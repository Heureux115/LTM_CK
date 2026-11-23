#define _POSIX_C_SOURCE 200809L

#include "server/core/router.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "server/services/auth.h"
#include "server/services/friends.h"
#include "server/services/groups.h"
#include "server/services/messages.h"
#include "server/services/password.h"
#include "server/repos/activity_repo.h"
#include "server/repos/group_repo.h"
#include "shared/protocol.h"

static RouterNotificationFn g_notification_dispatcher = NULL;
static void *g_notification_user_data = NULL;
static RouterDeliveryPushFn g_delivery_push_fn = NULL;
static RouterDeliveryAckFn g_delivery_ack_fn = NULL;
static void *g_delivery_user_data = NULL;

void router_set_notification_dispatcher(RouterNotificationFn fn, void *user_data)
{
    g_notification_dispatcher = fn;
    g_notification_user_data = user_data;
}

void router_set_delivery_hooks(RouterDeliveryPushFn push_fn,
                               RouterDeliveryAckFn ack_fn,
                               void *user_data)
{
    g_delivery_push_fn = push_fn;
    g_delivery_ack_fn = ack_fn;
    g_delivery_user_data = user_data;
}

void router_broadcast_presence(Database *db, const SessionManager *session_mgr,
                               int64_t user_id, bool online, const char *reason, int64_t now_ms)
{
    if (g_notification_dispatcher == NULL || db == NULL || session_mgr == NULL ||
        user_id <= 0 || reason == NULL) {
        return;
    }

    if (now_ms == 0) {
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
            now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        }
    }

    ByteBuffer event_buf = {0};
    if (buffer_init(&event_buf, 512) != BUFFER_OK) {
        return;
    }

    if (protocol_encode_presence_changed_event(user_id, online, reason, now_ms, &event_buf) != PROTOCOL_OK) {
        buffer_destroy(&event_buf);
        return;
    }

    int64_t after_id = 0;
    FriendRecord records[32];
    size_t count = 0;
    bool has_more = false;

    do {
        FriendRepoResult fres = friend_repo_list_friends(
            db, user_id, after_id, 32, records, &count, &has_more);
        if (fres != FRIEND_REPO_OK || count == 0) {
            break;
        }
        for (size_t i = 0; i < count; i++) {
            int64_t fid = records[i].friend_user_id;
            if (session_manager_is_user_online(session_mgr, fid)) {
                g_notification_dispatcher(g_notification_user_data, fid, &event_buf);
            }
            after_id = fid;
        }
    } while (has_more && count > 0);

    buffer_destroy(&event_buf);
}

static bool parse_json_id_field(const cJSON *item, int64_t *out_id)
{
    if (item == NULL) {
        return false;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *endptr = NULL;
        long long val = strtoll(item->valuestring, &endptr, 10);
        if (*endptr == '\0' && val > 0 && endptr != item->valuestring) {
            *out_id = (int64_t)val;
            return true;
        }
    } else if (cJSON_IsNumber(item) && item->valuedouble > 0 &&
               (double)((long long)item->valuedouble) == item->valuedouble) {
        *out_id = (int64_t)item->valuedouble;
        return true;
    }
    return false;
}

static bool parse_json_after_id_field(const cJSON *item, int64_t *out_after_id)
{
    if (item == NULL) {
        *out_after_id = 0;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        char *endptr = NULL;
        long long val = strtoll(item->valuestring, &endptr, 10);
        if (*endptr == '\0' && val >= 0 && endptr != item->valuestring) {
            *out_after_id = (int64_t)val;
            return true;
        }
    } else if (cJSON_IsNumber(item) && item->valuedouble >= 0 &&
               (double)((long long)item->valuedouble) == item->valuedouble) {
        *out_after_id = (int64_t)item->valuedouble;
        return true;
    }
    return false;
}

static bool parse_json_limit_field(const cJSON *item, int *out_limit, int default_limit)
{
    if (item == NULL) {
        *out_limit = default_limit;
        return true;
    }
    if (cJSON_IsNumber(item) && item->valuedouble >= 1 && item->valuedouble <= 32 &&
        (double)((int)item->valuedouble) == item->valuedouble) {
        *out_limit = (int)item->valuedouble;
        return true;
    }
    return false;
}

static ProtocolErrorCode map_group_service_error_code(GroupServiceResult res)
{
    switch (res) {
    case GROUP_SERVICE_VALIDATION_ERROR:
    case GROUP_SERVICE_INVALID_ARGUMENT:
        return PROTOCOL_ERROR_VALIDATION;
    case GROUP_SERVICE_IDEMPOTENCY_CONFLICT:
        return PROTOCOL_ERROR_IDEMPOTENCY_CONFLICT;
    case GROUP_SERVICE_NOT_FOUND:
        return PROTOCOL_ERROR_NOT_FOUND;
    case GROUP_SERVICE_FORBIDDEN:
        return PROTOCOL_ERROR_FORBIDDEN;
    case GROUP_SERVICE_CONFLICT:
        return PROTOCOL_ERROR_CONFLICT;
    case GROUP_SERVICE_GROUP_CLOSED:
        return PROTOCOL_ERROR_GROUP_CLOSED;
    case GROUP_SERVICE_LIMIT_EXCEEDED:
        return PROTOCOL_ERROR_LIMIT_EXCEEDED;
    case GROUP_SERVICE_STORAGE_ERROR:
        return PROTOCOL_ERROR_STORAGE;
    case GROUP_SERVICE_INTERNAL_ERROR:
    default:
        return PROTOCOL_ERROR_INTERNAL;
    }
}

static bool is_auth_required_type(const char *type)
{
    if (type == NULL) {
        return false;
    }
    if (strcmp(type, "auth.logout") == 0 ||
        strcmp(type, "delivery.ack") == 0 ||
        strncmp(type, "friend.", 7) == 0 ||
        strncmp(type, "direct.", 7) == 0 ||
        strncmp(type, "group.", 6) == 0) {
        return true;
    }
    return false;
}

static bool is_unauthenticated_only_type(const char *type)
{
    if (type == NULL) {
        return false;
    }
    return (strcmp(type, "auth.register") == 0 || strcmp(type, "auth.login") == 0);
}

static RouterResult encoded_result(ProtocolResult result, bool close_after_flush)
{
    switch (result) {
    case PROTOCOL_OK:
        return close_after_flush ? ROUTER_CLOSE_AFTER_FLUSH : ROUTER_OK;
    case PROTOCOL_OUTPUT_FULL:
        return ROUTER_OUTPUT_FULL;
    case PROTOCOL_INVALID_JSON:
    case PROTOCOL_INVALID_REQUEST:
    case PROTOCOL_UNSUPPORTED_VERSION:
    case PROTOCOL_ALLOCATION_ERROR:
        return ROUTER_INTERNAL_ERROR;
    }
    return ROUTER_INTERNAL_ERROR;
}

RouterResult router_handle_frame(const unsigned char *body, size_t length,
                                  ByteBuffer *output,
                                  RequestCache *cache,
                                  uint64_t *high_water_request_id,
                                  Database *db,
                                  SessionManager *session_mgr,
                                  const char *connection_id,
                                  uint64_t connection_generation)
{
    (void)connection_generation;
    ProtocolRequest request = {0};
    ProtocolResult parsed = protocol_parse_request(body, length, &request);
    if (parsed != PROTOCOL_OK) {
        if (parsed == PROTOCOL_UNSUPPORTED_VERSION) {
            ProtocolResult encoded = protocol_encode_error_response(
                &request, PROTOCOL_ERROR_UNSUPPORTED_VERSION, output);
            protocol_request_destroy(&request);
            return encoded_result(encoded, true);
        }
        if (parsed == PROTOCOL_ALLOCATION_ERROR) {
            return ROUTER_INTERNAL_ERROR;
        }
        ProtocolErrorCode code = parsed == PROTOCOL_INVALID_JSON ?
                                 PROTOCOL_ERROR_INVALID_JSON : PROTOCOL_ERROR_INVALID_REQUEST;
        return encoded_result(protocol_encode_protocol_error(code, output), true);
    }

    char *data_json = NULL;
    uint64_t req_id = 0;
    if (cache != NULL && high_water_request_id != NULL) {
        if (!protocol_parse_request_id(request.request_id, &req_id)) {
            protocol_request_destroy(&request);
            return encoded_result(protocol_encode_protocol_error(
                PROTOCOL_ERROR_INVALID_REQUEST, output), true);
        }
        data_json = cJSON_PrintUnformatted(request.data);
        if (data_json == NULL) {
            protocol_request_destroy(&request);
            return ROUTER_INTERNAL_ERROR;
        }
        const RequestCacheEntry *cached_entry = NULL;
        CacheLookupResult lookup = request_cache_check(cache, *high_water_request_id,
                                                       req_id, request.type, data_json,
                                                       &cached_entry);
        if (lookup == CACHE_LOOKUP_MATCH) {
            free(data_json);
            BufferResult app_res = buffer_append(output, cached_entry->response_frame,
                                                 cached_entry->response_frame_length);
            protocol_request_destroy(&request);
            return (app_res == BUFFER_OK) ? ROUTER_OK : ROUTER_OUTPUT_FULL;
        }
        if (lookup == CACHE_LOOKUP_REUSED) {
            free(data_json);
            ProtocolResult encoded = protocol_encode_error_response(
                &request, PROTOCOL_ERROR_REQUEST_ID_REUSED, output);
            protocol_request_destroy(&request);
            return encoded_result(encoded, false);
        }
        if (lookup == CACHE_LOOKUP_EXPIRED) {
            free(data_json);
            ProtocolResult encoded = protocol_encode_error_response(
                &request, PROTOCOL_ERROR_REQUEST_ID_EXPIRED, output);
            protocol_request_destroy(&request);
            return encoded_result(encoded, false);
        }
        /* lookup == CACHE_LOOKUP_NEW: request mới */
        *high_water_request_id = req_id;
    }

    /* Authentication Gate: Kiểm tra phân quyền wire protocol (A, U, B) */
    int64_t authenticated_user_id = 0;
    bool is_authenticated = false;
    if (session_mgr != NULL && connection_id != NULL) {
        is_authenticated = session_manager_is_connection_authenticated(
            session_mgr, connection_id, &authenticated_user_id);
    }

    /* Nhóm U (Unauthenticated only): từ chối nếu kết nối đã đăng nhập */
    if (is_authenticated && is_unauthenticated_only_type(request.type)) {
        ProtocolResult encoded = protocol_encode_error_response(
            &request, PROTOCOL_ERROR_ALREADY_AUTHENTICATED, output);
        if (data_json != NULL) {
            free(data_json);
            data_json = NULL;
        }
        protocol_request_destroy(&request);
        return encoded_result(encoded, false);
    }

    /* Nhóm A (Authenticated only): từ chối nếu kết nối chưa đăng nhập (không chạm nghiệp vụ) */
    if (!is_authenticated && is_auth_required_type(request.type)) {
        ProtocolResult encoded = protocol_encode_error_response(
            &request, PROTOCOL_ERROR_AUTH_REQUIRED, output);
        if (data_json != NULL) {
            free(data_json);
            data_json = NULL;
        }
        protocol_request_destroy(&request);
        return encoded_result(encoded, false);
    }

    /* Router nghiệp vụ đứng giữa parser JSON và queue socket. */
    size_t before_length = output != NULL ? output->length : 0;
    bool should_cache = true;
    RouterResult result;

    if (strcmp(request.type, "system.ping") == 0) {
        if (request.data->child != NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else {
            struct timespec now;
            if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
                result = ROUTER_INTERNAL_ERROR;
            } else {
                int64_t server_time_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                result = encoded_result(protocol_encode_ping_response(
                    &request, server_time_ms, output), false);
            }
        }
    } else if (strcmp(request.type, "connection.close") == 0) {
        if (request.data->child != NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else {
            if (session_mgr != NULL && connection_id != NULL) {
                int64_t unbind_uid = 0;
                SessionResult unbind_res = session_manager_unbind_by_connection(
                    session_mgr, connection_id, connection_generation, &unbind_uid);
                if (unbind_res == SESSION_OK && db != NULL) {
                    struct timespec now;
                    int64_t now_ms = 0;
                    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                        now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                    }
                    char uid_str[32];
                    snprintf(uid_str, sizeof(uid_str), "%lld", (long long)unbind_uid);
                    activity_repo_log(db, now_ms, ACTIVITY_LEVEL_INFO, &unbind_uid,
                                      connection_id, "connection.close", ACTIVITY_OUTCOME_SUCCESS,
                                      ACTIVITY_TARGET_CONNECTION, uid_str, NULL, NULL);
                    router_broadcast_presence(db, session_mgr, unbind_uid, false, "quit", now_ms);
                }
            }
            result = encoded_result(protocol_encode_close_response(&request, output), true);
        }
    } else if (strcmp(request.type, "auth.register") == 0) {
        /* Không lưu thông tin đăng ký chứa mật khẩu trong replay cache (W02) */
        should_cache = false;
        if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            cJSON *username_item = cJSON_GetObjectItemCaseSensitive(request.data, "username");
            cJSON *password_item = cJSON_GetObjectItemCaseSensitive(request.data, "password");
            if (!cJSON_IsString(username_item) || username_item->valuestring == NULL ||
                !cJSON_IsString(password_item) || password_item->valuestring == NULL ||
                cJSON_GetArraySize(request.data) != 2) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                struct timespec now;
                int64_t now_ms = 0;
                if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                    now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                }
                UserRecord new_user = {0};
                size_t pw_len = strlen(password_item->valuestring);
                AuthServiceResult auth_res = auth_service_register(
                    db,
                    username_item->valuestring,
                    password_item->valuestring,
                    pw_len,
                    connection_id,
                    now_ms,
                    &new_user);

                /* Xóa an toàn mật khẩu plaintext trong valuestring */
                password_wipe(password_item->valuestring, pw_len);

                if (auth_res == AUTH_SERVICE_OK) {
                    result = encoded_result(protocol_encode_register_response(
                        &request, new_user.user_id, new_user.username, output), false);
                } else if (auth_res == AUTH_SERVICE_USERNAME_TAKEN) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_USERNAME_TAKEN, output), false);
                } else if (auth_res == AUTH_SERVICE_VALIDATION_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else if (auth_res == AUTH_SERVICE_STORAGE_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_STORAGE, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_INTERNAL, output), false);
                }
            }
        }
    } else if (strcmp(request.type, "auth.login") == 0) {
        /* Không lưu thông tin đăng nhập chứa mật khẩu trong replay cache (W02, W03) */
        should_cache = false;
        if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else if (session_mgr == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_INTERNAL, output), false);
        } else {
            cJSON *username_item = cJSON_GetObjectItemCaseSensitive(request.data, "username");
            cJSON *password_item = cJSON_GetObjectItemCaseSensitive(request.data, "password");
            if (!cJSON_IsString(username_item) || username_item->valuestring == NULL ||
                !cJSON_IsString(password_item) || password_item->valuestring == NULL ||
                cJSON_GetArraySize(request.data) != 2) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                struct timespec now;
                int64_t now_ms = 0;
                if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                    now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                }
                UserRecord logged_in_user = {0};
                size_t pw_len = strlen(password_item->valuestring);
                AuthServiceResult auth_res = auth_service_login(
                    db,
                    session_mgr,
                    username_item->valuestring,
                    password_item->valuestring,
                    pw_len,
                    connection_id,
                    connection_generation,
                    now_ms,
                    &logged_in_user);

                /* Xóa an toàn mật khẩu plaintext trong valuestring */
                password_wipe(password_item->valuestring, pw_len);

                if (auth_res == AUTH_SERVICE_OK) {
                    result = encoded_result(protocol_encode_login_response(
                        &request, logged_in_user.user_id, logged_in_user.username,
                        CHAT_HEARTBEAT_IDLE_TIMEOUT_MS, CHAT_HEARTBEAT_PONG_TIMEOUT_MS,
                        output), false);
                    router_broadcast_presence(db, session_mgr, logged_in_user.user_id, true, "login", now_ms);
                } else if (auth_res == AUTH_SERVICE_AUTH_FAILED) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_AUTH_FAILED, output), false);
                } else if (auth_res == AUTH_SERVICE_ALREADY_ONLINE) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_ALREADY_ONLINE, output), false);
                } else if (auth_res == AUTH_SERVICE_ALREADY_AUTHENTICATED) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_ALREADY_AUTHENTICATED, output), false);
                } else if (auth_res == AUTH_SERVICE_VALIDATION_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else if (auth_res == AUTH_SERVICE_STORAGE_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_STORAGE, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_INTERNAL, output), false);
                }
            }
        }
    } else if (strcmp(request.type, "auth.logout") == 0) {
        /* Không lưu logout trong replay cache vì socket đóng ngay sau đó */
        should_cache = false;
        if (request.data->child != NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (session_mgr == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_INTERNAL, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            int64_t logged_out_uid = 0;
            AuthServiceResult auth_res = auth_service_logout(
                db, session_mgr, connection_id, connection_generation, now_ms, &logged_out_uid);
            (void)logged_out_uid;

            if (auth_res == AUTH_SERVICE_OK) {
                result = encoded_result(protocol_encode_logout_response(&request, output), true);
                if (logged_out_uid > 0) {
                    router_broadcast_presence(db, session_mgr, logged_out_uid, false, "logout", now_ms);
                }
            } else if (auth_res == AUTH_SERVICE_AUTH_FAILED) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_AUTH_REQUIRED, output), false);
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_INTERNAL, output), false);
            }
        }
    } else if (strcmp(request.type, "friend.request.send") == 0) {
        cJSON *username_item = cJSON_GetObjectItemCaseSensitive(request.data, "username");
        if (!cJSON_IsObject(request.data) || username_item == NULL ||
            !cJSON_IsString(username_item) || username_item->valuestring == NULL ||
            cJSON_GetArraySize(request.data) != 1) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            UserRecord actor_user = {0};
            UserRepoResult find_actor = user_repo_find_by_id(db, authenticated_user_id, &actor_user);
            if (find_actor != USER_REPO_OK) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_INTERNAL, output), false);
            } else {
                struct timespec now;
                int64_t now_ms = 0;
                if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                    now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                }
                FriendRequestRecord created_req = {0};
                UserRecord recipient_user = {0};
                FriendServiceResult fres = friend_service_send_request(
                    db,
                    authenticated_user_id,
                    actor_user.username,
                    username_item->valuestring,
                    connection_id,
                    now_ms,
                    &created_req,
                    &recipient_user);

                if (fres == FRIEND_SERVICE_OK) {
                    const char *status_str = friend_request_status_to_string(created_req.status);
                    result = encoded_result(
                        protocol_encode_friend_request_send_response(
                            &request,
                            created_req.friend_request_id,
                            created_req.sender_id,
                            actor_user.username,
                            created_req.recipient_id,
                            recipient_user.username,
                            status_str,
                            created_req.created_at,
                            output),
                        false);

                    /* Đẩy event friend.request.received cho người nhận nếu người nhận đang online */
                    if (g_notification_dispatcher != NULL) {
                        ByteBuffer event_buf = {0};
                        if (buffer_init(&event_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_friend_request_received_event(
                                    created_req.friend_request_id,
                                    created_req.sender_id,
                                    actor_user.username,
                                    created_req.recipient_id,
                                    recipient_user.username,
                                    status_str,
                                    created_req.created_at,
                                    &event_buf) == PROTOCOL_OK) {
                                g_notification_dispatcher(g_notification_user_data,
                                                          recipient_user.user_id,
                                                          &event_buf);
                            }
                            buffer_destroy(&event_buf);
                        }
                    }
                } else if (fres == FRIEND_SERVICE_VALIDATION_ERROR ||
                           fres == FRIEND_SERVICE_SELF_RELATION) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else if (fres == FRIEND_SERVICE_NOT_FOUND) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_NOT_FOUND, output), false);
                } else if (fres == FRIEND_SERVICE_ALREADY_FRIENDS ||
                           fres == FRIEND_SERVICE_PENDING_EXISTS) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_CONFLICT, output), false);
                } else if (fres == FRIEND_SERVICE_STORAGE_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_STORAGE, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_INTERNAL, output), false);
                }
            }
        }
    } else if (strcmp(request.type, "friend.request.list") == 0) {
        if (!cJSON_IsObject(request.data)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            cJSON *dir_item = cJSON_GetObjectItemCaseSensitive(request.data, "direction");
            cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(request.data, "limit");
            cJSON *after_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "after_id");

            int expected_keys = 1 + (limit_item != NULL ? 1 : 0) + (after_id_item != NULL ? 1 : 0);
            if (cJSON_GetArraySize(request.data) != expected_keys ||
                dir_item == NULL || !cJSON_IsString(dir_item) || dir_item->valuestring == NULL) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                const char *dir_str = dir_item->valuestring;
                FriendRequestDirection dir;
                bool dir_ok = true;
                if (strcmp(dir_str, "incoming") == 0) {
                    dir = FRIEND_REQUEST_DIR_INCOMING;
                } else if (strcmp(dir_str, "outgoing") == 0) {
                    dir = FRIEND_REQUEST_DIR_OUTGOING;
                } else {
                    dir_ok = false;
                }

                int limit = 32;
                bool limit_ok = true;
                if (limit_item != NULL) {
                    if (!cJSON_IsNumber(limit_item) || limit_item->valuedouble < 1 || limit_item->valuedouble > 32 ||
                        (double)((int)limit_item->valuedouble) != limit_item->valuedouble) {
                        limit_ok = false;
                    } else {
                        limit = (int)limit_item->valuedouble;
                    }
                }

                int64_t after_id = 0;
                bool after_id_ok = true;
                if (after_id_item != NULL) {
                    if (cJSON_IsString(after_id_item) && after_id_item->valuestring != NULL) {
                        char *endptr = NULL;
                        long long val = strtoll(after_id_item->valuestring, &endptr, 10);
                        if (*endptr != '\0' || val < 0 || endptr == after_id_item->valuestring) {
                            after_id_ok = false;
                        } else {
                            after_id = (int64_t)val;
                        }
                    } else if (cJSON_IsNumber(after_id_item) && after_id_item->valuedouble >= 0 &&
                               (double)((long long)after_id_item->valuedouble) == after_id_item->valuedouble) {
                        after_id = (int64_t)after_id_item->valuedouble;
                    } else {
                        after_id_ok = false;
                    }
                }

                if (!dir_ok || !limit_ok || !after_id_ok) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else {
                    FriendRequestRecord records[32];
                    size_t count = 0;
                    bool has_more = false;

                    FriendServiceResult sres = friend_service_list_requests(
                        db, authenticated_user_id, dir, after_id, (size_t)limit, records, &count, &has_more);

                    if (sres == FRIEND_SERVICE_OK) {
                        ProtocolFriendRequest proto_items[32];
                        for (size_t i = 0; i < count; i++) {
                            proto_items[i].friend_request_id = records[i].friend_request_id;
                            proto_items[i].sender_id = records[i].sender_id;
                            proto_items[i].sender_username = records[i].sender_username;
                            proto_items[i].recipient_id = records[i].recipient_id;
                            proto_items[i].recipient_username = records[i].recipient_username;
                            proto_items[i].status = friend_request_status_to_string(records[i].status);
                            proto_items[i].created_at = records[i].created_at;
                        }

                        int64_t next_after_id = 0;
                        if (has_more && count > 0) {
                            next_after_id = records[count - 1].friend_request_id;
                        }

                        result = encoded_result(
                            protocol_encode_friend_request_list_response(
                                &request, proto_items, count, has_more, next_after_id, output),
                            false);
                    } else if (sres == FRIEND_SERVICE_STORAGE_ERROR) {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_STORAGE, output), false);
                    } else {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_INTERNAL, output), false);
                    }
                }
            }
        }
    } else if (strcmp(request.type, "friend.request.accept") == 0) {
        cJSON *req_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "friend_request_id");
        if (!cJSON_IsObject(request.data) || req_id_item == NULL ||
            cJSON_GetArraySize(request.data) != 1) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            int64_t target_request_id = 0;
            bool id_ok = false;
            if (cJSON_IsString(req_id_item) && req_id_item->valuestring != NULL) {
                char *endptr = NULL;
                long long val = strtoll(req_id_item->valuestring, &endptr, 10);
                if (*endptr == '\0' && val > 0 && endptr != req_id_item->valuestring) {
                    target_request_id = (int64_t)val;
                    id_ok = true;
                }
            } else if (cJSON_IsNumber(req_id_item) && req_id_item->valuedouble > 0 &&
                       (double)((long long)req_id_item->valuedouble) == req_id_item->valuedouble) {
                target_request_id = (int64_t)req_id_item->valuedouble;
                id_ok = true;
            }

            if (!id_ok) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                struct timespec now;
                int64_t now_ms = 0;
                if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                    now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                }

                UserRecord sender_user = {0};
                UserRecord recipient_user = {0};

                FriendServiceResult fres = friend_service_accept_request(
                    db,
                    authenticated_user_id,
                    target_request_id,
                    connection_id,
                    now_ms,
                    &sender_user,
                    &recipient_user);

                if (fres == FRIEND_SERVICE_OK) {
                    bool sender_online = (session_mgr != NULL &&
                                          session_manager_is_user_online(session_mgr, sender_user.user_id));
                    bool recipient_online = true;

                    /* Caller (Bob) nhận response chứa thông tin người bạn mới (Alice) */
                    result = encoded_result(
                        protocol_encode_friend_request_accept_response(
                            &request,
                            sender_user.user_id,
                            sender_user.username,
                            sender_online,
                            output),
                        false);

                    /* Phát server push events nếu dispatcher được cấu hình */
                    if (g_notification_dispatcher != NULL) {
                        /* 1. Event friend.request.resolved ("accepted") gửi tới cả hai bên */
                        ByteBuffer res_buf = {0};
                        if (buffer_init(&res_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_friend_request_resolved_event(
                                    target_request_id,
                                    "accepted",
                                    authenticated_user_id,
                                    &res_buf) == PROTOCOL_OK) {
                                g_notification_dispatcher(g_notification_user_data,
                                                          sender_user.user_id,
                                                          &res_buf);
                                g_notification_dispatcher(g_notification_user_data,
                                                          recipient_user.user_id,
                                                          &res_buf);
                            }
                            buffer_destroy(&res_buf);
                        }

                        /* 2. Event friend.changed ("added") gửi tới sender (thông tin recipient) */
                        ByteBuffer chg_sender_buf = {0};
                        if (buffer_init(&chg_sender_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_friend_changed_event(
                                    "added",
                                    recipient_user.user_id,
                                    recipient_user.username,
                                    recipient_online,
                                    &chg_sender_buf) == PROTOCOL_OK) {
                                g_notification_dispatcher(g_notification_user_data,
                                                          sender_user.user_id,
                                                          &chg_sender_buf);
                            }
                            buffer_destroy(&chg_sender_buf);
                        }

                        /* 3. Event friend.changed ("added") gửi tới recipient (thông tin sender) */
                        ByteBuffer chg_recip_buf = {0};
                        if (buffer_init(&chg_recip_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_friend_changed_event(
                                    "added",
                                    sender_user.user_id,
                                    sender_user.username,
                                    sender_online,
                                    &chg_recip_buf) == PROTOCOL_OK) {
                                g_notification_dispatcher(g_notification_user_data,
                                                          recipient_user.user_id,
                                                          &chg_recip_buf);
                            }
                            buffer_destroy(&chg_recip_buf);
                        }
                    }
                } else if (fres == FRIEND_SERVICE_NOT_FOUND) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_NOT_FOUND, output), false);
                } else if (fres == FRIEND_SERVICE_FORBIDDEN) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_FORBIDDEN, output), false);
                } else if (fres == FRIEND_SERVICE_CONFLICT) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_CONFLICT, output), false);
                } else if (fres == FRIEND_SERVICE_STORAGE_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_STORAGE, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_INTERNAL, output), false);
                }
            }
        }
    } else if (strcmp(request.type, "friend.request.reject") == 0) {
        cJSON *req_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "friend_request_id");
        if (!cJSON_IsObject(request.data) || req_id_item == NULL ||
            cJSON_GetArraySize(request.data) != 1) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            int64_t target_request_id = 0;
            bool id_ok = false;
            if (cJSON_IsString(req_id_item) && req_id_item->valuestring != NULL) {
                char *endptr = NULL;
                long long val = strtoll(req_id_item->valuestring, &endptr, 10);
                if (*endptr == '\0' && val > 0 && endptr != req_id_item->valuestring) {
                    target_request_id = (int64_t)val;
                    id_ok = true;
                }
            } else if (cJSON_IsNumber(req_id_item) && req_id_item->valuedouble > 0 &&
                       (double)((long long)req_id_item->valuedouble) == req_id_item->valuedouble) {
                target_request_id = (int64_t)req_id_item->valuedouble;
                id_ok = true;
            }

            if (!id_ok) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                struct timespec now;
                int64_t now_ms = 0;
                if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                    now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                }

                int64_t sender_user_id = 0;
                FriendServiceResult fres = friend_service_reject_request(
                    db,
                    authenticated_user_id,
                    target_request_id,
                    connection_id,
                    now_ms,
                    &sender_user_id);

                if (fres == FRIEND_SERVICE_OK) {
                    /* Caller (Bob) nhận response status: "rejected" */
                    result = encoded_result(
                        protocol_encode_friend_request_reject_response(
                            &request,
                            target_request_id,
                            output),
                        false);

                    /* Phát server push event friend.request.resolved ("rejected") tới hai bên online */
                    if (g_notification_dispatcher != NULL) {
                        ByteBuffer res_buf = {0};
                        if (buffer_init(&res_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_friend_request_resolved_event(
                                    target_request_id,
                                    "rejected",
                                    authenticated_user_id,
                                    &res_buf) == PROTOCOL_OK) {
                                g_notification_dispatcher(g_notification_user_data,
                                                          sender_user_id,
                                                          &res_buf);
                                g_notification_dispatcher(g_notification_user_data,
                                                          authenticated_user_id,
                                                          &res_buf);
                            }
                            buffer_destroy(&res_buf);
                        }
                    }
                } else if (fres == FRIEND_SERVICE_NOT_FOUND) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_NOT_FOUND, output), false);
                } else if (fres == FRIEND_SERVICE_FORBIDDEN) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_FORBIDDEN, output), false);
                } else if (fres == FRIEND_SERVICE_CONFLICT) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_CONFLICT, output), false);
                } else if (fres == FRIEND_SERVICE_STORAGE_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_STORAGE, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_INTERNAL, output), false);
                }
            }
        }
    } else if (strcmp(request.type, "friend.remove") == 0) {
        cJSON *friend_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "friend_user_id");
        if (!cJSON_IsObject(request.data) || friend_id_item == NULL ||
            cJSON_GetArraySize(request.data) != 1) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            int64_t target_friend_id = 0;
            bool id_ok = false;
            if (cJSON_IsString(friend_id_item) && friend_id_item->valuestring != NULL) {
                char *endptr = NULL;
                long long val = strtoll(friend_id_item->valuestring, &endptr, 10);
                if (*endptr == '\0' && val > 0 && endptr != friend_id_item->valuestring) {
                    target_friend_id = (int64_t)val;
                    id_ok = true;
                }
            } else if (cJSON_IsNumber(friend_id_item) && friend_id_item->valuedouble > 0 &&
                       (double)((long long)friend_id_item->valuedouble) == friend_id_item->valuedouble) {
                target_friend_id = (int64_t)friend_id_item->valuedouble;
                id_ok = true;
            }

            if (!id_ok || target_friend_id == authenticated_user_id) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                struct timespec now;
                int64_t now_ms = 0;
                if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                    now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
                }

                UserRecord actor_user = {0};
                UserRecord friend_user = {0};
                FriendServiceResult fres = friend_service_remove_friend(
                    db,
                    authenticated_user_id,
                    target_friend_id,
                    connection_id,
                    now_ms,
                    &actor_user,
                    &friend_user);

                if (fres == FRIEND_SERVICE_OK) {
                    /* Caller nhận response removed: true */
                    result = encoded_result(
                        protocol_encode_friend_remove_response(
                            &request,
                            target_friend_id,
                            output),
                        false);

                    /* Phát server push event friend.changed (action: "removed") tới 2 bên online */
                    if (g_notification_dispatcher != NULL) {
                        bool actor_online = true;
                        bool friend_online = (session_mgr != NULL &&
                                              session_manager_is_user_online(session_mgr, target_friend_id));

                        /* 1. Event friend.changed gửi tới actor (thông tin friend_user) */
                        ByteBuffer act_buf = {0};
                        if (buffer_init(&act_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_friend_changed_event(
                                    "removed",
                                    friend_user.user_id,
                                    friend_user.username,
                                    friend_online,
                                    &act_buf) == PROTOCOL_OK) {
                                g_notification_dispatcher(g_notification_user_data,
                                                          authenticated_user_id,
                                                          &act_buf);
                            }
                            buffer_destroy(&act_buf);
                        }

                        /* 2. Event friend.changed gửi tới friend (thông tin actor_user) */
                        ByteBuffer fr_buf = {0};
                        if (buffer_init(&fr_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_friend_changed_event(
                                    "removed",
                                    actor_user.user_id,
                                    actor_user.username,
                                    actor_online,
                                    &fr_buf) == PROTOCOL_OK) {
                                g_notification_dispatcher(g_notification_user_data,
                                                          target_friend_id,
                                                          &fr_buf);
                            }
                            buffer_destroy(&fr_buf);
                        }
                    }
                } else if (fres == FRIEND_SERVICE_NOT_FOUND) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_NOT_FOUND, output), false);
                } else if (fres == FRIEND_SERVICE_SELF_RELATION ||
                           fres == FRIEND_SERVICE_VALIDATION_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else if (fres == FRIEND_SERVICE_STORAGE_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_STORAGE, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_INTERNAL, output), false);
                }
            }
        }
    } else if (strcmp(request.type, "friend.list") == 0) {
        if (!cJSON_IsObject(request.data)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(request.data, "limit");
            cJSON *after_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "after_id");

            int expected_keys = (limit_item != NULL ? 1 : 0) + (after_id_item != NULL ? 1 : 0);
            if (cJSON_GetArraySize(request.data) != expected_keys) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                int limit = 32;
                bool limit_ok = true;
                if (limit_item != NULL) {
                    if (!cJSON_IsNumber(limit_item) || limit_item->valuedouble < 1 || limit_item->valuedouble > 32 ||
                        (double)((int)limit_item->valuedouble) != limit_item->valuedouble) {
                        limit_ok = false;
                    } else {
                        limit = (int)limit_item->valuedouble;
                    }
                }

                int64_t after_id = 0;
                bool after_id_ok = true;
                if (after_id_item != NULL) {
                    if (cJSON_IsString(after_id_item) && after_id_item->valuestring != NULL) {
                        char *endptr = NULL;
                        long long val = strtoll(after_id_item->valuestring, &endptr, 10);
                        if (*endptr != '\0' || val < 0 || endptr == after_id_item->valuestring) {
                            after_id_ok = false;
                        } else {
                            after_id = (int64_t)val;
                        }
                    } else if (cJSON_IsNumber(after_id_item) && after_id_item->valuedouble >= 0 &&
                               (double)((long long)after_id_item->valuedouble) == after_id_item->valuedouble) {
                        after_id = (int64_t)after_id_item->valuedouble;
                    } else {
                        after_id_ok = false;
                    }
                }

                if (!limit_ok || !after_id_ok) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else {
                    FriendItem friend_items[32];
                    size_t count = 0;
                    bool has_more = false;

                    FriendServiceResult sres = friend_service_list_friends(
                        db, session_mgr, authenticated_user_id, after_id, (size_t)limit,
                        friend_items, &count, &has_more);

                    if (sres == FRIEND_SERVICE_OK) {
                        ProtocolFriend proto_items[32];
                        for (size_t i = 0; i < count; i++) {
                            proto_items[i].user_id = friend_items[i].user_id;
                            proto_items[i].username = friend_items[i].username;
                            proto_items[i].online = friend_items[i].online;
                        }

                        int64_t next_after_id = 0;
                        if (has_more && count > 0) {
                            next_after_id = friend_items[count - 1].user_id;
                        }

                        result = encoded_result(
                            protocol_encode_friend_list_response(
                                &request, proto_items, count, has_more, next_after_id, output),
                            false);
                    } else if (sres == FRIEND_SERVICE_STORAGE_ERROR) {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_STORAGE, output), false);
                    } else {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_INTERNAL, output), false);
                    }
                }
            }
        }
    } else if (strcmp(request.type, "direct.send") == 0) {
        cJSON *recip_item = cJSON_GetObjectItemCaseSensitive(request.data, "recipient_user_id");
        cJSON *cid_item = cJSON_GetObjectItemCaseSensitive(request.data, "client_msg_id");
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(request.data, "text");

        if (!cJSON_IsObject(request.data) || cJSON_GetArraySize(request.data) != 3 ||
            recip_item == NULL || cid_item == NULL || text_item == NULL ||
            !cJSON_IsString(cid_item) || !cJSON_IsString(text_item)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            int64_t recipient_id = 0;
            bool id_ok = false;
            if (cJSON_IsString(recip_item) && recip_item->valuestring != NULL) {
                char *endptr = NULL;
                long long val = strtoll(recip_item->valuestring, &endptr, 10);
                if (*endptr == '\0' && val > 0 && endptr != recip_item->valuestring) {
                    recipient_id = (int64_t)val;
                    id_ok = true;
                }
            } else if (cJSON_IsNumber(recip_item) && recip_item->valuedouble > 0 &&
                       (double)((long long)recip_item->valuedouble) == recip_item->valuedouble) {
                recipient_id = (int64_t)recip_item->valuedouble;
                id_ok = true;
            }

            const char *client_msg_id = cid_item->valuestring;
            const char *text = text_item->valuestring;

            if (!id_ok || recipient_id == authenticated_user_id ||
                !message_repo_validate_client_msg_id(client_msg_id) ||
                !message_repo_validate_body(text, NULL)) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                /* Lấy username của authenticated_user_id */
                UserRecord sender_user = {0};
                UserRepoResult ures = user_repo_find_by_id(db, authenticated_user_id, &sender_user);
                const char *sender_name = (ures == USER_REPO_OK) ? sender_user.username : "";

                DirectMessageRecord msg = {0};
                MessageServiceResult mres = message_service_send_direct(
                    db, authenticated_user_id, sender_name, recipient_id,
                    client_msg_id, text, connection_id, 0, &msg);

                if (mres == MESSAGE_SERVICE_OK || mres == MESSAGE_SERVICE_IDEMPOTENT_HIT) {
                    result = encoded_result(
                        protocol_encode_direct_send_response(
                            &request, "direct", msg.message_id, msg.client_msg_id,
                            msg.created_at, "stored", output),
                        false);

                    /* Chỉ push event direct.message khi là tin nhắn mới (MESSAGE_SERVICE_OK),
                     * không push lại khi idempotent hit (đã push lần đầu rồi) */
                    if (mres == MESSAGE_SERVICE_OK &&
                        (g_delivery_push_fn != NULL || g_notification_dispatcher != NULL)) {
                        ByteBuffer event_buf = {0};
                        if (buffer_init(&event_buf, CHAT_FRAME_HEADER_BYTES + CHAT_MAX_FRAME_BODY_BYTES) == BUFFER_OK) {
                            if (protocol_encode_direct_message_event(
                                    msg.message_id, msg.client_msg_id,
                                    authenticated_user_id, sender_name,
                                    recipient_id, text, msg.created_at,
                                    &event_buf) == PROTOCOL_OK) {
                                if (session_mgr == NULL ||
                                    session_manager_is_user_online(session_mgr, recipient_id)) {
                                    if (g_delivery_push_fn != NULL) {
                                        g_delivery_push_fn(g_delivery_user_data,
                                                           recipient_id,
                                                           "direct",
                                                           msg.message_id,
                                                           &event_buf);
                                    } else {
                                        g_notification_dispatcher(g_notification_user_data,
                                                                  recipient_id,
                                                                  &event_buf);
                                    }
                                }
                            }
                            buffer_destroy(&event_buf);
                        }
                    }
                } else if (mres == MESSAGE_SERVICE_IDEMPOTENCY_CONFLICT) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_IDEMPOTENCY_CONFLICT, output), false);
                } else if (mres == MESSAGE_SERVICE_FORBIDDEN) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_FORBIDDEN, output), false);
                } else if (mres == MESSAGE_SERVICE_NOT_FOUND) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_NOT_FOUND, output), false);
                } else if (mres == MESSAGE_SERVICE_VALIDATION_ERROR ||
                           mres == MESSAGE_SERVICE_SELF_SEND ||
                           mres == MESSAGE_SERVICE_INVALID_ARGUMENT) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else if (mres == MESSAGE_SERVICE_STORAGE_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_STORAGE, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_INTERNAL, output), false);
                }
            }
        }
    } else if (strcmp(request.type, "delivery.ack") == 0) {
        cJSON *msgs_item = cJSON_GetObjectItemCaseSensitive(request.data, "messages");

        if (!cJSON_IsObject(request.data) || cJSON_GetArraySize(request.data) != 1 ||
            msgs_item == NULL || !cJSON_IsArray(msgs_item)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            int count = cJSON_GetArraySize(msgs_item);
            if (count <= 0 || count > (int)CHAT_DELIVERY_ACK_MAX_ITEMS) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                MessageAckItem items[CHAT_DELIVERY_ACK_MAX_ITEMS];
                bool parse_ok = true;

                for (int i = 0; i < count; ++i) {
                    cJSON *elem = cJSON_GetArrayItem(msgs_item, i);
                    if (!cJSON_IsObject(elem) || cJSON_GetArraySize(elem) != 2) {
                        parse_ok = false;
                        break;
                    }
                    cJSON *scope_item = cJSON_GetObjectItemCaseSensitive(elem, "scope");
                    cJSON *mid_item = cJSON_GetObjectItemCaseSensitive(elem, "message_id");

                    if (scope_item == NULL || !cJSON_IsString(scope_item) || scope_item->valuestring == NULL ||
                        mid_item == NULL) {
                        parse_ok = false;
                        break;
                    }

                    int64_t mid_val = 0;
                    if (cJSON_IsString(mid_item) && mid_item->valuestring != NULL) {
                        char *endptr = NULL;
                        long long val = strtoll(mid_item->valuestring, &endptr, 10);
                        if (*endptr == '\0' && val > 0 && endptr != mid_item->valuestring) {
                            mid_val = (int64_t)val;
                        } else {
                            parse_ok = false;
                            break;
                        }
                    } else if (cJSON_IsNumber(mid_item) && mid_item->valuedouble > 0 &&
                               (double)((long long)mid_item->valuedouble) == mid_item->valuedouble) {
                        mid_val = (int64_t)mid_item->valuedouble;
                    } else {
                        parse_ok = false;
                        break;
                    }

                    snprintf(items[i].scope, sizeof(items[i].scope), "%s", scope_item->valuestring);
                    items[i].message_id = mid_val;
                }

                if (!parse_ok) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else {
                    MessageAckItem out_acked[CHAT_DELIVERY_ACK_MAX_ITEMS];
                    size_t out_count = 0;

                    MessageServiceResult mres = message_service_ack_delivery(
                        db, authenticated_user_id, items, (size_t)count,
                        connection_id, 0, out_acked, &out_count);

                    if (mres == MESSAGE_SERVICE_OK) {
                        result = encoded_result(
                            protocol_encode_delivery_ack_response(
                                &request, out_acked, out_count, output),
                            false);
                        if (result == ROUTER_OK && g_delivery_ack_fn != NULL) {
                            for (size_t i = 0; i < out_count; ++i) {
                                g_delivery_ack_fn(g_delivery_user_data,
                                                  connection_id,
                                                  connection_generation,
                                                  out_acked[i].scope,
                                                  out_acked[i].message_id);
                            }
                        }
                    } else if (mres == MESSAGE_SERVICE_NOT_FOUND) {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_NOT_FOUND, output), false);
                    } else if (mres == MESSAGE_SERVICE_FORBIDDEN) {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_FORBIDDEN, output), false);
                    } else if (mres == MESSAGE_SERVICE_VALIDATION_ERROR ||
                               mres == MESSAGE_SERVICE_INVALID_ARGUMENT) {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_VALIDATION, output), false);
                    } else if (mres == MESSAGE_SERVICE_STORAGE_ERROR) {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_STORAGE, output), false);
                    } else {
                        result = encoded_result(protocol_encode_error_response(
                            &request, PROTOCOL_ERROR_INTERNAL, output), false);
                    }
                }
            }
        }
    } else if (strcmp(request.type, "direct.history") == 0) {
        cJSON *peer_item = cJSON_GetObjectItemCaseSensitive(request.data, "peer_user_id");
        cJSON *after_item = cJSON_GetObjectItemCaseSensitive(request.data, "after_id");
        cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(request.data, "limit");

        int num_keys = cJSON_GetArraySize(request.data);
        int expected_keys = (peer_item != NULL ? 1 : 0) +
                            (after_item != NULL ? 1 : 0) +
                            (limit_item != NULL ? 1 : 0);

        if (!cJSON_IsObject(request.data) || num_keys != expected_keys || peer_item == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            int64_t peer_user_id = 0;
            bool valid_peer = false;

            if (cJSON_IsString(peer_item) && peer_item->valuestring != NULL) {
                char *endptr = NULL;
                long long val = strtoll(peer_item->valuestring, &endptr, 10);
                if (*endptr == '\0' && val > 0 && endptr != peer_item->valuestring) {
                    peer_user_id = (int64_t)val;
                    valid_peer = true;
                }
            } else if (cJSON_IsNumber(peer_item) && peer_item->valuedouble > 0 &&
                       (double)((long long)peer_item->valuedouble) == peer_item->valuedouble) {
                peer_user_id = (int64_t)peer_item->valuedouble;
                valid_peer = true;
            }

            int64_t after_id = 0;
            bool valid_after = true;
            if (after_item != NULL) {
                if (cJSON_IsString(after_item) && after_item->valuestring != NULL) {
                    char *endptr = NULL;
                    long long val = strtoll(after_item->valuestring, &endptr, 10);
                    if (*endptr == '\0' && val >= 0 && endptr != after_item->valuestring) {
                        after_id = (int64_t)val;
                    } else {
                        valid_after = false;
                    }
                } else if (cJSON_IsNumber(after_item) && after_item->valuedouble >= 0 &&
                           (double)((long long)after_item->valuedouble) == after_item->valuedouble) {
                    after_id = (int64_t)after_item->valuedouble;
                } else {
                    valid_after = false;
                }
            }

            int limit = 32;
            bool valid_limit = true;
            if (limit_item != NULL) {
                if (cJSON_IsNumber(limit_item) && limit_item->valuedouble >= 1 &&
                    limit_item->valuedouble <= 32 &&
                    (double)((int)limit_item->valuedouble) == limit_item->valuedouble) {
                    limit = (int)limit_item->valuedouble;
                } else {
                    valid_limit = false;
                }
            }

            if (!valid_peer || !valid_after || !valid_limit || peer_user_id == authenticated_user_id) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                DirectMessageRecord records[32];
                size_t out_count = 0;
                bool has_more = false;

                MessageServiceResult mres = message_service_list_history(
                    db, authenticated_user_id, peer_user_id, after_id, (size_t)limit,
                    records, &out_count, &has_more);

                if (mres == MESSAGE_SERVICE_OK) {
                    ProtocolDirectMessageItem proto_items[32];
                    int64_t next_after_id = 0;
                    for (size_t i = 0; i < out_count; ++i) {
                        proto_items[i].message_id = records[i].message_id;
                        proto_items[i].sender_id = records[i].sender_id;
                        snprintf(proto_items[i].sender_username, sizeof(proto_items[i].sender_username),
                                 "%s", records[i].sender_username);
                        proto_items[i].recipient_id = records[i].recipient_id;
                        snprintf(proto_items[i].recipient_username, sizeof(proto_items[i].recipient_username),
                                 "%s", records[i].recipient_username);
                        snprintf(proto_items[i].client_msg_id, sizeof(proto_items[i].client_msg_id),
                                 "%s", records[i].client_msg_id);
                        snprintf(proto_items[i].text, sizeof(proto_items[i].text),
                                 "%s", records[i].body);
                        proto_items[i].created_at = records[i].created_at;
                        proto_items[i].delivered_at = records[i].delivered_at;
                    }
                    if (out_count > 0) {
                        next_after_id = records[out_count - 1].message_id;
                    }

                    result = encoded_result(protocol_encode_direct_history_response(
                        &request, proto_items, out_count, has_more, next_after_id, output), false);
                } else if (mres == MESSAGE_SERVICE_NOT_FOUND) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_NOT_FOUND, output), false);
                } else if (mres == MESSAGE_SERVICE_FORBIDDEN) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_FORBIDDEN, output), false);
                } else if (mres == MESSAGE_SERVICE_SELF_SEND ||
                           mres == MESSAGE_SERVICE_VALIDATION_ERROR ||
                           mres == MESSAGE_SERVICE_INVALID_ARGUMENT) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_VALIDATION, output), false);
                } else if (mres == MESSAGE_SERVICE_STORAGE_ERROR) {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_STORAGE, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, PROTOCOL_ERROR_INTERNAL, output), false);
                }
            }
        }
    } else if (strcmp(request.type, "group.create") == 0) {
        cJSON *name_item = cJSON_GetObjectItemCaseSensitive(request.data, "name");
        cJSON *vis_item = cJSON_GetObjectItemCaseSensitive(request.data, "visibility");
        if (!cJSON_IsObject(request.data) || name_item == NULL || vis_item == NULL ||
            !cJSON_IsString(name_item) || name_item->valuestring == NULL ||
            !cJSON_IsString(vis_item) || vis_item->valuestring == NULL ||
            cJSON_GetArraySize(request.data) != 2) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            ChatGroupRecord created_grp = {0};
            GroupServiceResult gres = group_service_create_group(
                db, authenticated_user_id, name_item->valuestring, vis_item->valuestring,
                connection_id, now_ms, &created_grp);
            if (gres == GROUP_SERVICE_OK) {
                ProtocolGroup pg = {0};
                pg.group_id = created_grp.group_id;
                snprintf(pg.name, sizeof(pg.name), "%s", created_grp.name);
                snprintf(pg.visibility, sizeof(pg.visibility), "%s", created_grp.visibility);
                pg.owner_user_id = created_grp.owner_id;
                pg.member_count = 1;
                pg.created_at = created_grp.created_at;
                result = encoded_result(
                    protocol_encode_group_response(&request, &pg, output), false);
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else if (strcmp(request.type, "group.list") == 0) {
        if (!cJSON_IsObject(request.data)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(request.data, "limit");
            cJSON *after_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "after_id");
            int expected_keys = (limit_item != NULL ? 1 : 0) + (after_id_item != NULL ? 1 : 0);
            int limit = 32;
            int64_t after_id = 0;
            if (cJSON_GetArraySize(request.data) != expected_keys ||
                !parse_json_limit_field(limit_item, &limit, 32) ||
                !parse_json_after_id_field(after_id_item, &after_id)) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                ChatGroupRecord grps[32];
                size_t out_count = 0;
                bool has_more = false;
                GroupServiceResult gres = group_service_list_groups(
                    db, authenticated_user_id, after_id, (size_t)limit, grps, &out_count, &has_more);
                if (gres == GROUP_SERVICE_OK) {
                    ProtocolGroup proto_grps[32];
                    for (size_t i = 0; i < out_count; i++) {
                        proto_grps[i].group_id = grps[i].group_id;
                        snprintf(proto_grps[i].name, sizeof(proto_grps[i].name), "%s", grps[i].name);
                        snprintf(proto_grps[i].visibility, sizeof(proto_grps[i].visibility), "%s", grps[i].visibility);
                        proto_grps[i].owner_user_id = grps[i].owner_id;
                        size_t mcount = 0;
                        group_repo_get_member_count(db, grps[i].group_id, &mcount);
                        proto_grps[i].member_count = (int32_t)mcount;
                        proto_grps[i].created_at = grps[i].created_at;
                    }
                    int64_t next_after_id = (out_count > 0) ? grps[out_count - 1].group_id : 0;
                    result = encoded_result(
                        protocol_encode_group_list_response(&request, proto_grps, out_count, has_more, next_after_id, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, map_group_service_error_code(gres), output), false);
                }
            }
        }
    } else if (strcmp(request.type, "group.closed.list") == 0) {
        if (!cJSON_IsObject(request.data)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(request.data, "limit");
            cJSON *after_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "after_id");
            int expected_keys = (limit_item != NULL ? 1 : 0) + (after_id_item != NULL ? 1 : 0);
            int limit = 32;
            int64_t after_id = 0;
            if (cJSON_GetArraySize(request.data) != expected_keys ||
                !parse_json_limit_field(limit_item, &limit, 32) ||
                !parse_json_after_id_field(after_id_item, &after_id)) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                ChatGroupRecord grps[32];
                size_t out_count = 0;
                bool has_more = false;
                GroupServiceResult gres = group_service_list_closed_groups(
                    db, authenticated_user_id, after_id, (size_t)limit, grps, &out_count, &has_more);
                if (gres == GROUP_SERVICE_OK) {
                    ProtocolClosedGroup proto_closed[32];
                    for (size_t i = 0; i < out_count; i++) {
                        proto_closed[i].group_id = grps[i].group_id;
                        snprintf(proto_closed[i].group_name, sizeof(proto_closed[i].group_name), "%s", grps[i].name);
                        proto_closed[i].by_user_id = grps[i].owner_id;
                        proto_closed[i].closed_at = grps[i].closed_at;
                        snprintf(proto_closed[i].reason, sizeof(proto_closed[i].reason), "owner_left");
                    }
                    int64_t next_after_id = (out_count > 0) ? grps[out_count - 1].group_id : 0;
                    result = encoded_result(
                        protocol_encode_group_closed_list_response(&request, proto_closed, out_count, has_more, next_after_id, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, map_group_service_error_code(gres), output), false);
                }
            }
        }
    } else if (strcmp(request.type, "group.members") == 0) {
        cJSON *gid_item = cJSON_GetObjectItemCaseSensitive(request.data, "group_id");
        cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(request.data, "limit");
        cJSON *after_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "after_id");
        int expected_keys = 1 + (limit_item != NULL ? 1 : 0) + (after_id_item != NULL ? 1 : 0);
        int64_t target_gid = 0;
        int limit = 32;
        int64_t after_id = 0;
        if (!cJSON_IsObject(request.data) || gid_item == NULL ||
            cJSON_GetArraySize(request.data) != expected_keys ||
            !parse_json_id_field(gid_item, &target_gid) ||
            !parse_json_limit_field(limit_item, &limit, 32) ||
            !parse_json_after_id_field(after_id_item, &after_id)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            GroupMemberRecord members[32];
            size_t out_count = 0;
            bool has_more = false;
            GroupServiceResult gres = group_service_list_members(
                db, authenticated_user_id, target_gid, after_id, (size_t)limit, members, &out_count, &has_more);
            if (gres == GROUP_SERVICE_OK) {
                ProtocolGroupMember proto_mems[32];
                for (size_t i = 0; i < out_count; i++) {
                    proto_mems[i].user_id = members[i].user_id;
                    snprintf(proto_mems[i].username, sizeof(proto_mems[i].username), "%s", members[i].username);
                    snprintf(proto_mems[i].role, sizeof(proto_mems[i].role), "%s", members[i].is_owner ? "owner" : "member");
                    proto_mems[i].joined_at = members[i].joined_at;
                }
                int64_t next_after_id = (out_count > 0) ? members[out_count - 1].user_id : 0;
                result = encoded_result(
                    protocol_encode_group_members_response(&request, proto_mems, out_count, has_more, next_after_id, output), false);
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else if (strcmp(request.type, "group.invite.send") == 0) {
        cJSON *gid_item = cJSON_GetObjectItemCaseSensitive(request.data, "group_id");
        cJSON *username_item = cJSON_GetObjectItemCaseSensitive(request.data, "username");
        int64_t target_gid = 0;
        if (!cJSON_IsObject(request.data) || gid_item == NULL || username_item == NULL ||
            !cJSON_IsString(username_item) || username_item->valuestring == NULL ||
            cJSON_GetArraySize(request.data) != 2 ||
            !parse_json_id_field(gid_item, &target_gid)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            GroupInvitationRecord inv = {0};
            UserRecord rec_user = {0};
            GroupServiceResult gres = group_service_send_invitation(
                db, authenticated_user_id, target_gid, username_item->valuestring,
                connection_id, now_ms, &inv, &rec_user);
            if (gres == GROUP_SERVICE_OK) {
                ProtocolGroupInvitation proto_inv = {0};
                proto_inv.invitation_id = inv.invitation_id;
                proto_inv.group_id = inv.group_id;
                snprintf(proto_inv.group_name, sizeof(proto_inv.group_name), "%s", inv.group_name);
                proto_inv.inviter_user_id = inv.inviter_id;
                snprintf(proto_inv.inviter_username, sizeof(proto_inv.inviter_username), "%s", inv.inviter_username);
                proto_inv.invitee_user_id = inv.invitee_id;
                snprintf(proto_inv.invitee_username, sizeof(proto_inv.invitee_username), "%s", inv.invitee_username);
                snprintf(proto_inv.status, sizeof(proto_inv.status), "%s", inv.status);
                proto_inv.created_at = inv.created_at;

                result = encoded_result(
                    protocol_encode_group_invite_send_response(&request, &proto_inv, output), false);

                /* Server push event group.invite.received tới invitee nếu online */
                if (g_notification_dispatcher != NULL) {
                    ByteBuffer ev_buf = {0};
                    if (buffer_init(&ev_buf, 512) == BUFFER_OK) {
                        if (protocol_encode_group_invite_received_event(&proto_inv, &ev_buf) == PROTOCOL_OK) {
                            g_notification_dispatcher(g_notification_user_data, rec_user.user_id, &ev_buf);
                        }
                        buffer_destroy(&ev_buf);
                    }
                }
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else if (strcmp(request.type, "group.invite.list") == 0) {
        if (!cJSON_IsObject(request.data)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            cJSON *limit_item = cJSON_GetObjectItemCaseSensitive(request.data, "limit");
            cJSON *after_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "after_id");
            int expected_keys = (limit_item != NULL ? 1 : 0) + (after_id_item != NULL ? 1 : 0);
            int limit = 32;
            int64_t after_id = 0;
            if (cJSON_GetArraySize(request.data) != expected_keys ||
                !parse_json_limit_field(limit_item, &limit, 32) ||
                !parse_json_after_id_field(after_id_item, &after_id)) {
                result = encoded_result(protocol_encode_error_response(
                    &request, PROTOCOL_ERROR_VALIDATION, output), false);
            } else {
                GroupInvitationRecord invs[32];
                size_t out_count = 0;
                bool has_more = false;
                GroupServiceResult gres = group_service_list_pending_invitations(
                    db, authenticated_user_id, after_id, (size_t)limit, invs, &out_count, &has_more);
                if (gres == GROUP_SERVICE_OK) {
                    ProtocolGroupInvitation proto_invs[32];
                    for (size_t i = 0; i < out_count; i++) {
                        proto_invs[i].invitation_id = invs[i].invitation_id;
                        proto_invs[i].group_id = invs[i].group_id;
                        snprintf(proto_invs[i].group_name, sizeof(proto_invs[i].group_name), "%s", invs[i].group_name);
                        proto_invs[i].inviter_user_id = invs[i].inviter_id;
                        snprintf(proto_invs[i].inviter_username, sizeof(proto_invs[i].inviter_username), "%s", invs[i].inviter_username);
                        proto_invs[i].invitee_user_id = invs[i].invitee_id;
                        snprintf(proto_invs[i].invitee_username, sizeof(proto_invs[i].invitee_username), "%s", invs[i].invitee_username);
                        snprintf(proto_invs[i].status, sizeof(proto_invs[i].status), "%s", invs[i].status);
                        proto_invs[i].created_at = invs[i].created_at;
                    }
                    int64_t next_after_id = (out_count > 0) ? invs[out_count - 1].invitation_id : 0;
                    result = encoded_result(
                        protocol_encode_group_invite_list_response(&request, proto_invs, out_count, has_more, next_after_id, output), false);
                } else {
                    result = encoded_result(protocol_encode_error_response(
                        &request, map_group_service_error_code(gres), output), false);
                }
            }
        }
    } else if (strcmp(request.type, "group.invite.accept") == 0) {
        cJSON *inv_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "invitation_id");
        int64_t target_inv_id = 0;
        if (!cJSON_IsObject(request.data) || inv_id_item == NULL ||
            cJSON_GetArraySize(request.data) != 1 ||
            !parse_json_id_field(inv_id_item, &target_inv_id)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            ChatGroupRecord joined_grp = {0};
            GroupInvitationRecord inv_rec = {0};
            size_t member_count = 0;
            GroupServiceResult gres = group_service_accept_invitation(
                db, authenticated_user_id, target_inv_id, connection_id, now_ms,
                &joined_grp, &inv_rec, &member_count);
            if (gres == GROUP_SERVICE_OK) {
                ProtocolGroup pg = {0};
                pg.group_id = joined_grp.group_id;
                snprintf(pg.name, sizeof(pg.name), "%s", joined_grp.name);
                snprintf(pg.visibility, sizeof(pg.visibility), "%s", joined_grp.visibility);
                pg.owner_user_id = joined_grp.owner_id;
                pg.member_count = (int32_t)member_count;
                pg.created_at = joined_grp.created_at;

                result = encoded_result(
                    protocol_encode_group_response(&request, &pg, output), false);

                if (g_notification_dispatcher != NULL) {
                    /* 1. group.invite.resolved gửi tới inviter và invitee online */
                    ByteBuffer res_buf = {0};
                    if (buffer_init(&res_buf, 512) == BUFFER_OK) {
                        if (protocol_encode_group_invite_resolved_event(
                                target_inv_id, joined_grp.group_id, "accepted",
                                authenticated_user_id, &res_buf) == PROTOCOL_OK) {
                            g_notification_dispatcher(g_notification_user_data, inv_rec.inviter_id, &res_buf);
                            g_notification_dispatcher(g_notification_user_data, inv_rec.invitee_id, &res_buf);
                        }
                        buffer_destroy(&res_buf);
                    }

                    /* 2. group.member.changed ("joined") gửi tới toàn bộ member của nhóm */
                    ByteBuffer mem_buf = {0};
                    if (buffer_init(&mem_buf, 512) == BUFFER_OK) {
                        if (protocol_encode_group_member_changed_event(
                                joined_grp.group_id, authenticated_user_id, inv_rec.invitee_username,
                                "joined", authenticated_user_id, (int32_t)member_count, &mem_buf) == PROTOCOL_OK) {
                            GroupMemberRecord all_mems[CHAT_GROUP_MAX_MEMBERS];
                            size_t all_count = 0;
                            bool dummy_more = false;
                            if (group_repo_list_members(db, joined_grp.group_id, 0, CHAT_GROUP_MAX_MEMBERS,
                                                        all_mems, &all_count, &dummy_more) == GROUP_REPO_OK) {
                                for (size_t mi = 0; mi < all_count; mi++) {
                                    g_notification_dispatcher(g_notification_user_data, all_mems[mi].user_id, &mem_buf);
                                }
                            }
                        }
                        buffer_destroy(&mem_buf);
                    }
                }
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else if (strcmp(request.type, "group.invite.reject") == 0) {
        cJSON *inv_id_item = cJSON_GetObjectItemCaseSensitive(request.data, "invitation_id");
        int64_t target_inv_id = 0;
        if (!cJSON_IsObject(request.data) || inv_id_item == NULL ||
            cJSON_GetArraySize(request.data) != 1 ||
            !parse_json_id_field(inv_id_item, &target_inv_id)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            GroupInvitationRecord inv_rec = {0};
            GroupServiceResult gres = group_service_reject_invitation(
                db, authenticated_user_id, target_inv_id, connection_id, now_ms, &inv_rec);
            if (gres == GROUP_SERVICE_OK) {
                result = encoded_result(
                    protocol_encode_group_invite_reject_response(&request, target_inv_id, output), false);

                if (g_notification_dispatcher != NULL) {
                    ByteBuffer res_buf = {0};
                    if (buffer_init(&res_buf, 512) == BUFFER_OK) {
                        if (protocol_encode_group_invite_resolved_event(
                                target_inv_id, inv_rec.group_id, "rejected",
                                authenticated_user_id, &res_buf) == PROTOCOL_OK) {
                            g_notification_dispatcher(g_notification_user_data, inv_rec.inviter_id, &res_buf);
                            g_notification_dispatcher(g_notification_user_data, inv_rec.invitee_id, &res_buf);
                        }
                        buffer_destroy(&res_buf);
                    }
                }
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else if (strcmp(request.type, "group.member.remove") == 0) {
        cJSON *gid_item = cJSON_GetObjectItemCaseSensitive(request.data, "group_id");
        cJSON *uid_item = cJSON_GetObjectItemCaseSensitive(request.data, "member_user_id");
        int64_t target_gid = 0;
        int64_t target_uid = 0;
        if (!cJSON_IsObject(request.data) || gid_item == NULL || uid_item == NULL ||
            cJSON_GetArraySize(request.data) != 2 ||
            !parse_json_id_field(gid_item, &target_gid) ||
            !parse_json_id_field(uid_item, &target_uid)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            UserRecord removed_user = {0};
            size_t new_member_count = 0;
            GroupServiceResult gres = group_service_remove_member(
                db, authenticated_user_id, target_gid, target_uid, connection_id, now_ms,
                &removed_user, &new_member_count);
            if (gres == GROUP_SERVICE_OK) {
                result = encoded_result(
                    protocol_encode_group_member_remove_response(&request, target_gid, target_uid, output), false);

                if (g_notification_dispatcher != NULL) {
                    ByteBuffer mem_buf = {0};
                    if (buffer_init(&mem_buf, 512) == BUFFER_OK) {
                        if (protocol_encode_group_member_changed_event(
                                target_gid, target_uid, removed_user.username, "removed",
                                authenticated_user_id, (int32_t)new_member_count, &mem_buf) == PROTOCOL_OK) {
                            GroupMemberRecord all_mems[CHAT_GROUP_MAX_MEMBERS];
                            size_t all_count = 0;
                            bool dummy_more = false;
                            if (group_repo_list_members(db, target_gid, 0, CHAT_GROUP_MAX_MEMBERS,
                                                        all_mems, &all_count, &dummy_more) == GROUP_REPO_OK) {
                                for (size_t mi = 0; mi < all_count; mi++) {
                                    g_notification_dispatcher(g_notification_user_data, all_mems[mi].user_id, &mem_buf);
                                }
                            }
                            g_notification_dispatcher(g_notification_user_data, target_uid, &mem_buf);
                        }
                        buffer_destroy(&mem_buf);
                    }
                }
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else if (strcmp(request.type, "group.owner.transfer") == 0) {
        cJSON *gid_item = cJSON_GetObjectItemCaseSensitive(request.data, "group_id");
        cJSON *uid_item = cJSON_GetObjectItemCaseSensitive(request.data, "new_owner_user_id");
        int64_t target_gid = 0;
        int64_t new_owner_id = 0;
        if (!cJSON_IsObject(request.data) || gid_item == NULL || uid_item == NULL ||
            cJSON_GetArraySize(request.data) != 2 ||
            !parse_json_id_field(gid_item, &target_gid) ||
            !parse_json_id_field(uid_item, &new_owner_id)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            ChatGroupRecord updated_grp = {0};
            GroupServiceResult gres = group_service_transfer_ownership(
                db, authenticated_user_id, target_gid, new_owner_id, connection_id, now_ms, &updated_grp);
            if (gres == GROUP_SERVICE_OK) {
                ProtocolGroup pg = {0};
                pg.group_id = updated_grp.group_id;
                snprintf(pg.name, sizeof(pg.name), "%s", updated_grp.name);
                snprintf(pg.visibility, sizeof(pg.visibility), "%s", updated_grp.visibility);
                pg.owner_user_id = updated_grp.owner_id;
                size_t mcount = 0;
                group_repo_get_member_count(db, target_gid, &mcount);
                pg.member_count = (int32_t)mcount;
                pg.created_at = updated_grp.created_at;

                result = encoded_result(
                    protocol_encode_group_response(&request, &pg, output), false);

                if (g_notification_dispatcher != NULL) {
                    ByteBuffer own_buf = {0};
                    if (buffer_init(&own_buf, 512) == BUFFER_OK) {
                        if (protocol_encode_group_owner_changed_event(
                                target_gid, authenticated_user_id, new_owner_id, now_ms, &own_buf) == PROTOCOL_OK) {
                            GroupMemberRecord all_mems[CHAT_GROUP_MAX_MEMBERS];
                            size_t all_count = 0;
                            bool dummy_more = false;
                            if (group_repo_list_members(db, target_gid, 0, CHAT_GROUP_MAX_MEMBERS,
                                                        all_mems, &all_count, &dummy_more) == GROUP_REPO_OK) {
                                for (size_t mi = 0; mi < all_count; mi++) {
                                    g_notification_dispatcher(g_notification_user_data, all_mems[mi].user_id, &own_buf);
                                }
                            }
                        }
                        buffer_destroy(&own_buf);
                    }
                }
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else if (strcmp(request.type, "group.leave") == 0) {
        cJSON *gid_item = cJSON_GetObjectItemCaseSensitive(request.data, "group_id");
        int64_t target_gid = 0;
        if (!cJSON_IsObject(request.data) || gid_item == NULL ||
            cJSON_GetArraySize(request.data) != 1 ||
            !parse_json_id_field(gid_item, &target_gid)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            bool closed = false;
            ChatGroupRecord grp = {0};
            GroupMemberRecord snapshot_members[CHAT_GROUP_MAX_MEMBERS];
            size_t snapshot_count = 0;
            size_t new_member_count = 0;
            GroupServiceResult gres = group_service_leave(
                db, authenticated_user_id, target_gid, connection_id, now_ms,
                &closed, &grp, snapshot_members, &snapshot_count, &new_member_count);
            if (gres == GROUP_SERVICE_OK) {
                result = encoded_result(
                    protocol_encode_group_leave_response(&request, target_gid, closed, output), false);

                if (g_notification_dispatcher != NULL) {
                    if (closed) {
                        /* Phát group.closed tới mọi member online trong snapshot (gồm cả owner) */
                        ByteBuffer close_buf = {0};
                        if (buffer_init(&close_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_group_closed_event(
                                    target_gid, grp.name, authenticated_user_id, now_ms, &close_buf) == PROTOCOL_OK) {
                                for (size_t si = 0; si < snapshot_count; si++) {
                                    g_notification_dispatcher(g_notification_user_data, snapshot_members[si].user_id, &close_buf);
                                }
                            }
                            buffer_destroy(&close_buf);
                        }
                    } else {
                        /* Phát group.member.changed (action: "left") tới thành viên còn lại và người rời */
                        UserRecord actor_user = {0};
                        user_repo_find_by_id(db, authenticated_user_id, &actor_user);
                        ByteBuffer mem_buf = {0};
                        if (buffer_init(&mem_buf, 512) == BUFFER_OK) {
                            if (protocol_encode_group_member_changed_event(
                                    target_gid, authenticated_user_id, actor_user.username,
                                    "left", authenticated_user_id, (int32_t)new_member_count, &mem_buf) == PROTOCOL_OK) {
                                GroupMemberRecord rem_mems[CHAT_GROUP_MAX_MEMBERS];
                                size_t rem_count = 0;
                                bool dummy_more = false;
                                if (group_repo_list_members(db, target_gid, 0, CHAT_GROUP_MAX_MEMBERS,
                                                            rem_mems, &rem_count, &dummy_more) == GROUP_REPO_OK) {
                                    for (size_t ri = 0; ri < rem_count; ri++) {
                                        g_notification_dispatcher(g_notification_user_data, rem_mems[ri].user_id, &mem_buf);
                                    }
                                }
                                g_notification_dispatcher(g_notification_user_data, authenticated_user_id, &mem_buf);
                            }
                            buffer_destroy(&mem_buf);
                        }
                    }
                }
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else if (strcmp(request.type, "group.send") == 0) {
        cJSON *gid_item = cJSON_GetObjectItemCaseSensitive(request.data, "group_id");
        cJSON *cid_item = cJSON_GetObjectItemCaseSensitive(request.data, "client_msg_id");
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(request.data, "text");

        int64_t target_gid = 0;
        if (!cJSON_IsObject(request.data) || gid_item == NULL || cid_item == NULL || text_item == NULL ||
            cJSON_GetArraySize(request.data) != 3 ||
            !parse_json_id_field(gid_item, &target_gid) ||
            !cJSON_IsString(cid_item) || !cJSON_IsString(text_item) ||
            !message_repo_validate_client_msg_id(cid_item->valuestring) ||
            !message_repo_validate_body(text_item->valuestring, NULL)) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_VALIDATION, output), false);
        } else if (db == NULL) {
            result = encoded_result(protocol_encode_error_response(
                &request, PROTOCOL_ERROR_STORAGE, output), false);
        } else {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            GroupMessageRecord msg = {0};
            int64_t recipient_ids[CHAT_GROUP_MAX_MEMBERS];
            size_t recipient_count = 0;
            GroupServiceResult gres = group_service_send_message(
                db, authenticated_user_id, target_gid,
                cid_item->valuestring, text_item->valuestring,
                connection_id, now_ms,
                &msg, recipient_ids, CHAT_GROUP_MAX_MEMBERS, &recipient_count);

            if (gres == GROUP_SERVICE_OK || gres == GROUP_SERVICE_IDEMPOTENT_HIT) {
                result = encoded_result(
                    protocol_encode_group_send_response(
                        &request, "group", msg.message_id, msg.client_msg_id,
                        msg.created_at, "stored", output),
                    false);

                if (gres == GROUP_SERVICE_OK &&
                    (g_delivery_push_fn != NULL || g_notification_dispatcher != NULL)) {
                    ByteBuffer event_buf = {0};
                    if (buffer_init(&event_buf, CHAT_FRAME_HEADER_BYTES + CHAT_MAX_FRAME_BODY_BYTES) == BUFFER_OK) {
                        if (protocol_encode_group_message_event(
                                msg.message_id, msg.client_msg_id,
                                msg.group_id, msg.group_name,
                                msg.sender_id, msg.sender_username,
                                msg.body, msg.created_at, &event_buf) == PROTOCOL_OK) {
                            for (size_t ri = 0; ri < recipient_count; ri++) {
                                if (session_mgr != NULL &&
                                    !session_manager_is_user_online(session_mgr, recipient_ids[ri])) {
                                    continue;
                                }
                                if (g_delivery_push_fn != NULL) {
                                    g_delivery_push_fn(g_delivery_user_data,
                                                       recipient_ids[ri],
                                                       "group",
                                                       msg.message_id,
                                                       &event_buf);
                                } else {
                                    g_notification_dispatcher(
                                        g_notification_user_data, recipient_ids[ri], &event_buf);
                                }
                            }
                        }
                        buffer_destroy(&event_buf);
                    }
                }
            } else {
                result = encoded_result(protocol_encode_error_response(
                    &request, map_group_service_error_code(gres), output), false);
            }
        }
    } else {
        result = encoded_result(protocol_encode_error_response(
            &request, PROTOCOL_ERROR_UNKNOWN_TYPE, output), false);
    }

    if (should_cache && cache != NULL && data_json != NULL &&
        (result == ROUTER_OK || result == ROUTER_CLOSE_AFTER_FLUSH)) {
        size_t frame_len = output->length - before_length;
        request_cache_put(cache, req_id, request.type, data_json,
                          output->data + before_length, frame_len);
    }
    if (data_json != NULL) {
        if (!should_cache) {
            password_wipe(data_json, strlen(data_json));
        }
        free(data_json);
        data_json = NULL;
    }
    protocol_request_destroy(&request);
    return result;
}
