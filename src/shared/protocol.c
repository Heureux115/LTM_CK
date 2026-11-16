#define _GNU_SOURCE

#include "shared/protocol.h"

#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include "shared/frame.h"
#include "shared/limits.h"

static bool continuation(unsigned char byte)
{
    return byte >= 0x80 && byte <= 0xbf;
}

static bool valid_utf8(const unsigned char *bytes, size_t length)
{
    for (size_t index = 0; index < length;) {
        unsigned char first = bytes[index];
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        size_t following = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            following = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            following = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            following = 3;
        } else {
            return false;
        }
        if (following > length - index - 1) {
            return false;
        }
        unsigned char second = bytes[index + 1];
        if (!continuation(second) ||
            (first == 0xe0 && second < 0xa0) ||
            (first == 0xed && second > 0x9f) ||
            (first == 0xf0 && second < 0x90) ||
            (first == 0xf4 && second > 0x8f)) {
            return false;
        }
        for (size_t offset = 2; offset <= following; ++offset) {
            if (!continuation(bytes[index + offset])) {
                return false;
            }
        }
        index += following + 1;
    }
    return true;
}

static ProtocolResult check_raw_json(const unsigned char *body, size_t length)
{
    if ((length >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) ||
        memchr(body, '\0', length) != NULL || !valid_utf8(body, length)) {
        return PROTOCOL_INVALID_JSON;
    }

    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t index = 0; index < length; ++index) {
        unsigned char byte = body[index];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (byte == '\\') {
                /* cJSON lưu chuỗi kiểu C: từ chối \u0000 trước khi parse. */
                if (index + 5 < length && body[index + 1] == 'u' &&
                    memcmp(body + index + 2, "0000", 4) == 0) {
                    return PROTOCOL_INVALID_JSON;
                }
                escaped = true;
            } else if (byte == '"') {
                in_string = false;
            }
        } else if (byte == '"') {
            in_string = true;
        } else if (byte == '{' || byte == '[') {
            if (++depth > 8) {
                return PROTOCOL_INVALID_REQUEST;
            }
        } else if (byte == '}' || byte == ']') {
            if (depth == 0) {
                return PROTOCOL_INVALID_JSON;
            }
            --depth;
        }
    }
    return PROTOCOL_OK;
}

static ProtocolResult check_tree(const cJSON *item, size_t *total_keys)
{
    if (cJSON_IsNumber(item) && !isfinite(item->valuedouble)) {
        return PROTOCOL_INVALID_JSON;
    }
    if (cJSON_IsObject(item)) {
        size_t object_keys = 0;
        for (const cJSON *child = item->child; child != NULL; child = child->next) {
            if (++object_keys > 32 || ++*total_keys > 512 || child->string == NULL) {
                return PROTOCOL_INVALID_REQUEST;
            }
            for (const cJSON *earlier = item->child; earlier != child;
                 earlier = earlier->next) {
                if (strcmp(earlier->string, child->string) == 0) {
                    return PROTOCOL_INVALID_REQUEST;
                }
            }
            ProtocolResult result = check_tree(child, total_keys);
            if (result != PROTOCOL_OK) {
                return result;
            }
        }
    } else if (cJSON_IsArray(item)) {
        for (const cJSON *child = item->child; child != NULL; child = child->next) {
            ProtocolResult result = check_tree(child, total_keys);
            if (result != PROTOCOL_OK) {
                return result;
            }
        }
    }
    return PROTOCOL_OK;
}

bool protocol_parse_request_id(const char *request_id, uint64_t *id_out)
{
    if (request_id == NULL || request_id[0] < '1' || request_id[0] > '9') {
        return false;
    }
    uint64_t parsed = 0;
    for (const char *digit = request_id; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }
        unsigned value = (unsigned)(*digit - '0');
        if (parsed > (UINT64_MAX - value) / 10) {
            return false;
        }
        parsed = parsed * 10 + value;
    }
    if (id_out != NULL) {
        *id_out = parsed;
    }
    return true;
}

static bool valid_request_id(const char *request_id)
{
    return protocol_parse_request_id(request_id, NULL);
}

static ProtocolResult parse_document(const unsigned char *body, size_t length,
                                     cJSON **document)
{
    if (body == NULL || document == NULL || *document != NULL || length == 0 ||
        length > CHAT_MAX_FRAME_BODY_BYTES) {
        return PROTOCOL_INVALID_REQUEST;
    }
    ProtocolResult result = check_raw_json(body, length);
    if (result != PROTOCOL_OK) {
        return result;
    }
    char *json = malloc(length + 1);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    memcpy(json, body, length);
    json[length] = '\0';
    cJSON *parsed = cJSON_ParseWithLengthOpts(json, length + 1, NULL, true);
    free(json);
    if (parsed == NULL) {
        return PROTOCOL_INVALID_JSON;
    }
    size_t total_keys = 0;
    result = check_tree(parsed, &total_keys);
    if (result != PROTOCOL_OK) {
        cJSON_Delete(parsed);
        return result;
    }
    *document = parsed;
    return PROTOCOL_OK;
}

ProtocolResult protocol_parse_request(const unsigned char *body, size_t length,
                                       ProtocolRequest *request)
{
    if (body == NULL || request == NULL || request->document != NULL ||
        request->type != NULL || request->request_id != NULL || request->data != NULL ||
        length == 0 || length > CHAT_MAX_FRAME_BODY_BYTES) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *document = NULL;
    ProtocolResult result = parse_document(body, length, &document);
    if (result != PROTOCOL_OK) {
        return result;
    }
    if (!cJSON_IsObject(document) || cJSON_GetArraySize(document) != 5) {
        cJSON_Delete(document);
        return PROTOCOL_INVALID_REQUEST;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(document, "v");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(document, "kind");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(document, "type");
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(document, "request_id");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(document, "data");
    if (!cJSON_IsNumber(version) || version->valuedouble < 0 ||
        version->valuedouble >= 9007199254740992.0 ||
        (double)(uint64_t)version->valuedouble != version->valuedouble ||
        !cJSON_IsString(kind) || strcmp(kind->valuestring, "request") != 0 ||
         !cJSON_IsString(type) || type->valuestring[0] == '\0' ||
         !cJSON_IsString(request_id) || !valid_request_id(request_id->valuestring) ||
         !cJSON_IsObject(data)) {
        result = PROTOCOL_INVALID_REQUEST;
    }
    if (result == PROTOCOL_OK) {
        /* Giữ envelope tin cậy dù version lạ, để response lỗi giữ request_id. */
        request->document = document;
        request->type = type->valuestring;
        request->request_id = request_id->valuestring;
        request->data = data;
        if (version->valuedouble != 1) {
            result = PROTOCOL_UNSUPPORTED_VERSION;
        }
    } else {
        cJSON_Delete(document);
    }
    return result;
}

void protocol_request_destroy(ProtocolRequest *request)
{
    if (request == NULL) {
        return;
    }
    cJSON_Delete(request->document);
    *request = (ProtocolRequest){0};
}

static ProtocolResult encode_success_response(const ProtocolRequest *request, cJSON *data,
                                              ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        request->request_id == NULL || data == NULL || output == NULL ||
        output->data == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    /* Hai response thành công dùng chung envelope; data chuyển quyền sở hữu cho cJSON. */
    bool complete = cJSON_AddNumberToObject(response, "v", 1) != NULL &&
                    cJSON_AddStringToObject(response, "kind", "response") != NULL &&
                    cJSON_AddStringToObject(response, "type", request->type) != NULL &&
                    cJSON_AddStringToObject(response, "request_id", request->request_id) != NULL &&
                    cJSON_AddBoolToObject(response, "ok", true) != NULL;
    if (complete) {
        complete = cJSON_AddItemToObject(response, "data", data);
        if (complete) {
            data = NULL;
        }
    }
    cJSON_Delete(data);
    if (!complete) {
        cJSON_Delete(response);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    size_t length = strlen(json);
    FrameResult frame_result = frame_encode(output, json, length);
    cJSON_free(json);
    return frame_result == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}

ProtocolResult protocol_encode_ping_response(const ProtocolRequest *request,
                                              int64_t server_time_ms, ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "system.ping") != 0 || request->request_id == NULL ||
        !cJSON_IsObject(request->data) || request->data->child != NULL ||
        server_time_ms < 0 || server_time_ms > 9007199254740991LL ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (cJSON_AddNumberToObject(data, "server_time", (double)server_time_ms) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_close_response(const ProtocolRequest *request,
                                               ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "connection.close") != 0 || request->request_id == NULL ||
        !cJSON_IsObject(request->data) || request->data->child != NULL ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (cJSON_AddBoolToObject(data, "closing", true) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_register_response(const ProtocolRequest *request,
                                                 int64_t user_id,
                                                 const char *username,
                                                 ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "auth.register") != 0 || request->request_id == NULL ||
        username == NULL || user_id <= 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *user = cJSON_CreateObject();
    if (user == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)user_id);
    if (cJSON_AddStringToObject(user, "user_id", id_str) == NULL ||
        cJSON_AddStringToObject(user, "username", username) == NULL ||
        !cJSON_AddItemToObject(data, "user", user)) {
        cJSON_Delete(user);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_login_response(const ProtocolRequest *request,
                                              int64_t user_id,
                                              const char *username,
                                              int64_t heartbeat_idle_ms,
                                              int64_t heartbeat_timeout_ms,
                                              ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "auth.login") != 0 || request->request_id == NULL ||
        username == NULL || user_id <= 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *user = cJSON_CreateObject();
    if (user == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)user_id);
    if (cJSON_AddStringToObject(user, "user_id", id_str) == NULL ||
        cJSON_AddStringToObject(user, "username", username) == NULL ||
        !cJSON_AddItemToObject(data, "user", user) ||
        cJSON_AddNumberToObject(data, "heartbeat_idle_ms", (double)heartbeat_idle_ms) == NULL ||
        cJSON_AddNumberToObject(data, "heartbeat_timeout_ms", (double)heartbeat_timeout_ms) == NULL) {
        cJSON_Delete(user);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_logout_response(const ProtocolRequest *request,
                                               ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "auth.logout") != 0 || request->request_id == NULL ||
        !cJSON_IsObject(request->data) || request->data->child != NULL ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (cJSON_AddBoolToObject(data, "logged_out", true) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

static cJSON *create_friend_request_json(int64_t friend_request_id,
                                         int64_t sender_id,
                                         const char *sender_username,
                                         int64_t recipient_id,
                                         const char *recipient_username,
                                         const char *status,
                                         int64_t created_at)
{
    cJSON *req = cJSON_CreateObject();
    if (req == NULL) {
        return NULL;
    }
    char req_id_str[32];
    snprintf(req_id_str, sizeof(req_id_str), "%lld", (long long)friend_request_id);

    cJSON *sender = cJSON_CreateObject();
    cJSON *recipient = cJSON_CreateObject();
    if (sender == NULL || recipient == NULL) {
        cJSON_Delete(sender);
        cJSON_Delete(recipient);
        cJSON_Delete(req);
        return NULL;
    }
    char s_id_str[32], r_id_str[32];
    snprintf(s_id_str, sizeof(s_id_str), "%lld", (long long)sender_id);
    snprintf(r_id_str, sizeof(r_id_str), "%lld", (long long)recipient_id);

    if (cJSON_AddStringToObject(sender, "user_id", s_id_str) == NULL ||
        cJSON_AddStringToObject(sender, "username", sender_username) == NULL ||
        cJSON_AddStringToObject(recipient, "user_id", r_id_str) == NULL ||
        cJSON_AddStringToObject(recipient, "username", recipient_username) == NULL ||
        cJSON_AddStringToObject(req, "friend_request_id", req_id_str) == NULL ||
        !cJSON_AddItemToObject(req, "sender", sender) ||
        !cJSON_AddItemToObject(req, "recipient", recipient) ||
        cJSON_AddStringToObject(req, "status", status != NULL ? status : "pending") == NULL ||
        cJSON_AddNumberToObject(req, "created_at", (double)created_at) == NULL) {
        cJSON_Delete(req);
        return NULL;
    }
    return req;
}

ProtocolResult protocol_encode_friend_request_send_response(const ProtocolRequest *request,
                                                             int64_t friend_request_id,
                                                             int64_t sender_id,
                                                             const char *sender_username,
                                                             int64_t recipient_id,
                                                             const char *recipient_username,
                                                             const char *status,
                                                             int64_t created_at,
                                                             ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "friend.request.send") != 0 || request->request_id == NULL ||
        sender_username == NULL || recipient_username == NULL ||
        sender_id <= 0 || recipient_id <= 0 || friend_request_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *req = create_friend_request_json(friend_request_id, sender_id, sender_username,
                                           recipient_id, recipient_username, status, created_at);
    if (req == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (!cJSON_AddItemToObject(data, "request", req)) {
        cJSON_Delete(req);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_friend_request_list_response(const ProtocolRequest *request,
                                                             const ProtocolFriendRequest *requests,
                                                             size_t request_count,
                                                             bool has_more,
                                                             int64_t next_after_id,
                                                             ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "friend.request.list") != 0 || request->request_id == NULL ||
        (request_count > 0 && requests == NULL) ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }

    cJSON *items = cJSON_CreateArray();
    if (items == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    for (size_t i = 0; i < request_count; i++) {
        cJSON *req = create_friend_request_json(
            requests[i].friend_request_id,
            requests[i].sender_id,
            requests[i].sender_username,
            requests[i].recipient_id,
            requests[i].recipient_username,
            requests[i].status,
            requests[i].created_at);
        if (req == NULL) {
            cJSON_Delete(items);
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
        if (!cJSON_AddItemToArray(items, req)) {
            cJSON_Delete(req);
            cJSON_Delete(items);
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    }

    cJSON *requests_copy = cJSON_Duplicate(items, 1);
    if (requests_copy == NULL) {
        cJSON_Delete(items);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    if (!cJSON_AddItemToObject(data, "items", items) ||
        !cJSON_AddItemToObject(data, "requests", requests_copy) ||
        cJSON_AddBoolToObject(data, "has_more", has_more) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    if (has_more && request_count > 0 && next_after_id > 0) {
        char next_str[32];
        snprintf(next_str, sizeof(next_str), "%lld", (long long)next_after_id);
        if (cJSON_AddStringToObject(data, "next_after_id", next_str) == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    } else {
        if (cJSON_AddNullToObject(data, "next_after_id") == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    }

    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_friend_request_received_event(int64_t friend_request_id,
                                                             int64_t sender_id,
                                                             const char *sender_username,
                                                             int64_t recipient_id,
                                                             const char *recipient_username,
                                                             const char *status,
                                                             int64_t created_at,
                                                             ByteBuffer *output)
{
    if (sender_username == NULL || recipient_username == NULL ||
        sender_id <= 0 || recipient_id <= 0 || friend_request_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *req = create_friend_request_json(friend_request_id, sender_id, sender_username,
                                           recipient_id, recipient_username, status, created_at);
    if (req == NULL) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (!cJSON_AddItemToObject(data, "request", req) ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "friend.request.received") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}





static cJSON *create_friend_json(int64_t user_id, const char *username, bool online)
{
    cJSON *friend_obj = cJSON_CreateObject();
    if (friend_obj == NULL) {
        return NULL;
    }
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)user_id);
    if (cJSON_AddStringToObject(friend_obj, "user_id", id_str) == NULL ||
        cJSON_AddStringToObject(friend_obj, "username", username != NULL ? username : "") == NULL ||
        cJSON_AddBoolToObject(friend_obj, "online", online) == NULL) {
        cJSON_Delete(friend_obj);
        return NULL;
    }
    return friend_obj;
}

ProtocolResult protocol_encode_friend_request_accept_response(const ProtocolRequest *request,
                                                              int64_t friend_user_id,
                                                              const char *friend_username,
                                                              bool friend_online,
                                                              ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "friend.request.accept") != 0 || request->request_id == NULL ||
        friend_username == NULL || friend_user_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *friend_obj = create_friend_json(friend_user_id, friend_username, friend_online);
    if (friend_obj == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (!cJSON_AddItemToObject(data, "friend", friend_obj)) {
        cJSON_Delete(friend_obj);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_friend_request_reject_response(const ProtocolRequest *request,
                                                              int64_t friend_request_id,
                                                              ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "friend.request.reject") != 0 || request->request_id == NULL ||
        friend_request_id <= 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char req_id_str[32];
    snprintf(req_id_str, sizeof(req_id_str), "%lld", (long long)friend_request_id);
    if (cJSON_AddStringToObject(data, "friend_request_id", req_id_str) == NULL ||
        cJSON_AddStringToObject(data, "status", "rejected") == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_friend_remove_response(const ProtocolRequest *request,
                                                      int64_t friend_user_id,
                                                      ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "friend.remove") != 0 || request->request_id == NULL ||
        friend_user_id <= 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)friend_user_id);
    if (cJSON_AddStringToObject(data, "friend_user_id", id_str) == NULL ||
        cJSON_AddBoolToObject(data, "removed", 1) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_friend_list_response(const ProtocolRequest *request,
                                                    const ProtocolFriend *friends,
                                                    size_t friend_count,
                                                    bool has_more,
                                                    int64_t next_after_id,
                                                    ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "friend.list") != 0 || request->request_id == NULL ||
        (friend_count > 0 && friends == NULL) ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }

    cJSON *items = cJSON_CreateArray();
    if (items == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    for (size_t i = 0; i < friend_count; i++) {
        cJSON *friend_obj = create_friend_json(friends[i].user_id,
                                               friends[i].username,
                                               friends[i].online);
        if (friend_obj == NULL) {
            cJSON_Delete(items);
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
        if (!cJSON_AddItemToArray(items, friend_obj)) {
            cJSON_Delete(friend_obj);
            cJSON_Delete(items);
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    }

    cJSON *friends_copy = cJSON_Duplicate(items, 1);
    if (friends_copy == NULL) {
        cJSON_Delete(items);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    if (!cJSON_AddItemToObject(data, "items", items) ||
        !cJSON_AddItemToObject(data, "friends", friends_copy) ||
        cJSON_AddBoolToObject(data, "has_more", has_more) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    if (has_more && friend_count > 0 && next_after_id > 0) {
        char next_str[32];
        snprintf(next_str, sizeof(next_str), "%lld", (long long)next_after_id);
        if (cJSON_AddStringToObject(data, "next_after_id", next_str) == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    } else {
        if (cJSON_AddNullToObject(data, "next_after_id") == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    }

    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_friend_request_resolved_event(int64_t friend_request_id,
                                                             const char *status,
                                                             int64_t by_user_id,
                                                             ByteBuffer *output)
{
    if (status == NULL || friend_request_id <= 0 || by_user_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char req_id_str[32], by_id_str[32];
    snprintf(req_id_str, sizeof(req_id_str), "%lld", (long long)friend_request_id);
    snprintf(by_id_str, sizeof(by_id_str), "%lld", (long long)by_user_id);

    if (cJSON_AddStringToObject(data, "friend_request_id", req_id_str) == NULL ||
        cJSON_AddStringToObject(data, "status", status) == NULL ||
        cJSON_AddStringToObject(data, "by_user_id", by_id_str) == NULL ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "friend.request.resolved") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}





ProtocolResult protocol_encode_friend_changed_event(const char *action,
                                                    int64_t friend_user_id,
                                                    const char *friend_username,
                                                    bool friend_online,
                                                    ByteBuffer *output)
{
    if (action == NULL || friend_username == NULL || friend_user_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *friend_obj = create_friend_json(friend_user_id, friend_username, friend_online);
    if (friend_obj == NULL) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (cJSON_AddStringToObject(data, "action", action) == NULL ||
        !cJSON_AddItemToObject(data, "friend", friend_obj) ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "friend.changed") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}





ProtocolResult protocol_encode_presence_changed_event(int64_t user_id,
                                                      bool online,
                                                      const char *reason,
                                                      int64_t at_ms,
                                                      ByteBuffer *output)
{
    if (user_id <= 0 || reason == NULL || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char user_id_str[32];
    snprintf(user_id_str, sizeof(user_id_str), "%lld", (long long)user_id);
    if (cJSON_AddStringToObject(data, "user_id", user_id_str) == NULL ||
        cJSON_AddBoolToObject(data, "online", online) == NULL ||
        cJSON_AddStringToObject(data, "reason", reason) == NULL ||
        cJSON_AddNumberToObject(data, "at", (double)at_ms) == NULL ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "presence.changed") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}





ProtocolResult protocol_encode_direct_send_response(const ProtocolRequest *request,
                                                    const char *scope,
                                                    int64_t message_id,
                                                    const char *client_msg_id,
                                                    int64_t created_at,
                                                    const char *status,
                                                    ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "direct.send") != 0 || request->request_id == NULL ||
        scope == NULL || message_id <= 0 || client_msg_id == NULL || created_at < 0 ||
        status == NULL || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char msg_id_str[32];
    snprintf(msg_id_str, sizeof(msg_id_str), "%lld", (long long)message_id);

    bool complete = cJSON_AddNumberToObject(root, "v", 1.0) != NULL &&
                    cJSON_AddStringToObject(root, "kind", "response") != NULL &&
                    cJSON_AddStringToObject(root, "type", "direct.send") != NULL &&
                    cJSON_AddStringToObject(root, "request_id", request->request_id) != NULL &&
                    cJSON_AddBoolToObject(root, "ok", true) != NULL &&
                    cJSON_AddStringToObject(data, "scope", scope) != NULL &&
                    cJSON_AddStringToObject(data, "message_id", msg_id_str) != NULL &&
                    cJSON_AddStringToObject(data, "client_msg_id", client_msg_id) != NULL &&
                    cJSON_AddNumberToObject(data, "created_at", (double)created_at) != NULL &&
                    cJSON_AddStringToObject(data, "status", status) != NULL;

    if (complete) {
        complete = cJSON_AddItemToObject(root, "data", data);
        if (complete) {
            data = NULL;
        }
    }
    cJSON_Delete(data);
    if (!complete) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}





ProtocolResult protocol_encode_direct_message_event(int64_t message_id,
                                                    const char *client_msg_id,
                                                    int64_t sender_id,
                                                    const char *sender_username,
                                                    int64_t recipient_id,
                                                    const char *text,
                                                    int64_t created_at,
                                                    ByteBuffer *output)
{
    if (message_id <= 0 || client_msg_id == NULL || sender_id <= 0 ||
        sender_username == NULL || recipient_id <= 0 || text == NULL ||
        created_at < 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *sender = cJSON_CreateObject();
    if (root == NULL || data == NULL || sender == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(sender);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char msg_id_str[32];
    snprintf(msg_id_str, sizeof(msg_id_str), "%lld", (long long)message_id);

    char sender_id_str[32];
    snprintf(sender_id_str, sizeof(sender_id_str), "%lld", (long long)sender_id);

    char recipient_id_str[32];
    snprintf(recipient_id_str, sizeof(recipient_id_str), "%lld", (long long)recipient_id);

    bool complete = cJSON_AddStringToObject(sender, "user_id", sender_id_str) != NULL &&
                    cJSON_AddStringToObject(sender, "username", sender_username) != NULL;

    if (complete) {
        complete = cJSON_AddItemToObject(data, "sender", sender);
        if (complete) {
            sender = NULL;
        }
    }
    cJSON_Delete(sender);

    complete = complete &&
               cJSON_AddNumberToObject(root, "v", 1.0) != NULL &&
               cJSON_AddStringToObject(root, "kind", "event") != NULL &&
               cJSON_AddStringToObject(root, "type", "direct.message") != NULL &&
               cJSON_AddStringToObject(data, "message_id", msg_id_str) != NULL &&
               cJSON_AddStringToObject(data, "client_msg_id", client_msg_id) != NULL &&
               cJSON_AddStringToObject(data, "recipient_user_id", recipient_id_str) != NULL &&
               cJSON_AddStringToObject(data, "text", text) != NULL &&
               cJSON_AddNumberToObject(data, "created_at", (double)created_at) != NULL;

    if (complete) {
        complete = cJSON_AddItemToObject(root, "data", data);
        if (complete) {
            data = NULL;
        }
    }
    cJSON_Delete(data);

    if (!complete) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_encode_delivery_ack_response(const ProtocolRequest *request,
                                                     const ProtocolAckItem *items,
                                                     size_t count,
                                                     ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "delivery.ack") != 0 || request->request_id == NULL ||
        (count > 0 && items == NULL) || count > CHAT_DELIVERY_ACK_MAX_ITEMS ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *acked = cJSON_CreateArray();
    if (root == NULL || data == NULL || acked == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(acked);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    bool complete = true;
    for (size_t i = 0; i < count; ++i) {
        cJSON *item_obj = cJSON_CreateObject();
        if (item_obj == NULL) {
            complete = false;
            break;
        }
        char mid_str[32];
        snprintf(mid_str, sizeof(mid_str), "%lld", (long long)items[i].message_id);
        if (cJSON_AddStringToObject(item_obj, "scope", items[i].scope) == NULL ||
            cJSON_AddStringToObject(item_obj, "message_id", mid_str) == NULL) {
            cJSON_Delete(item_obj);
            complete = false;
            break;
        }
        cJSON_AddItemToArray(acked, item_obj);
    }

    if (complete) {
        complete = cJSON_AddNumberToObject(root, "v", 1.0) != NULL &&
                   cJSON_AddStringToObject(root, "kind", "response") != NULL &&
                   cJSON_AddStringToObject(root, "type", "delivery.ack") != NULL &&
                   cJSON_AddStringToObject(root, "request_id", request->request_id) != NULL &&
                   cJSON_AddBoolToObject(root, "ok", true) != NULL &&
                   cJSON_AddItemToObject(data, "acked", acked) &&
                   cJSON_AddItemToObject(root, "data", data);
        if (complete) {
            data = NULL;
            acked = NULL;
        }
    }

    cJSON_Delete(data);
    cJSON_Delete(acked);
    if (!complete) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_encode_direct_history_response(const ProtocolRequest *request,
                                                       const ProtocolDirectMessageItem *items,
                                                       size_t item_count,
                                                       bool has_more,
                                                       int64_t next_after_id,
                                                       ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "direct.history") != 0 || request->request_id == NULL ||
        (item_count > 0 && items == NULL) ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *items_arr = cJSON_CreateArray();
    if (root == NULL || data == NULL || items_arr == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(items_arr);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    bool complete = true;
    for (size_t i = 0; i < item_count; ++i) {
        cJSON *msg_obj = cJSON_CreateObject();
        if (msg_obj == NULL) {
            complete = false;
            break;
        }
        char msg_id_str[32];
        char sender_id_str[32];
        char recipient_id_str[32];
        snprintf(msg_id_str, sizeof(msg_id_str), "%lld", (long long)items[i].message_id);
        snprintf(sender_id_str, sizeof(sender_id_str), "%lld", (long long)items[i].sender_id);
        snprintf(recipient_id_str, sizeof(recipient_id_str), "%lld", (long long)items[i].recipient_id);

        if (cJSON_AddStringToObject(msg_obj, "message_id", msg_id_str) == NULL ||
            cJSON_AddStringToObject(msg_obj, "sender_id", sender_id_str) == NULL ||
            cJSON_AddStringToObject(msg_obj, "sender_username", items[i].sender_username) == NULL ||
            cJSON_AddStringToObject(msg_obj, "recipient_id", recipient_id_str) == NULL ||
            cJSON_AddStringToObject(msg_obj, "recipient_username", items[i].recipient_username) == NULL ||
            cJSON_AddStringToObject(msg_obj, "client_msg_id", items[i].client_msg_id) == NULL ||
            cJSON_AddStringToObject(msg_obj, "text", items[i].text) == NULL ||
            cJSON_AddNumberToObject(msg_obj, "created_at", (double)items[i].created_at) == NULL ||
            cJSON_AddNumberToObject(msg_obj, "delivered_at", (double)items[i].delivered_at) == NULL) {
            cJSON_Delete(msg_obj);
            complete = false;
            break;
        }
        cJSON_AddItemToArray(items_arr, msg_obj);
    }

    if (complete) {
        complete = cJSON_AddNumberToObject(root, "v", 1.0) != NULL &&
                   cJSON_AddStringToObject(root, "kind", "response") != NULL &&
                   cJSON_AddStringToObject(root, "type", "direct.history") != NULL &&
                   cJSON_AddStringToObject(root, "request_id", request->request_id) != NULL &&
                   cJSON_AddBoolToObject(root, "ok", true) != NULL &&
                   cJSON_AddItemToObject(data, "items", items_arr) &&
                   cJSON_AddBoolToObject(data, "has_more", has_more) != NULL;

        if (has_more && item_count > 0 && next_after_id > 0) {
            char next_str[32];
            snprintf(next_str, sizeof(next_str), "%lld", (long long)next_after_id);
            complete = complete && (cJSON_AddStringToObject(data, "next_after_id", next_str) != NULL);
        } else {
            complete = complete && (cJSON_AddNullToObject(data, "next_after_id") != NULL);
        }

        complete = complete && cJSON_AddItemToObject(root, "data", data);
        if (complete) {
            data = NULL;
            items_arr = NULL;
        }
    }

    cJSON_Delete(data);
    cJSON_Delete(items_arr);
    if (!complete) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



static cJSON *create_group_json(const ProtocolGroup *group)
{
    if (group == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    char gid_str[32], oid_str[32];
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)group->group_id);
    snprintf(oid_str, sizeof(oid_str), "%lld", (long long)group->owner_user_id);

    if (cJSON_AddStringToObject(obj, "group_id", gid_str) == NULL ||
        cJSON_AddStringToObject(obj, "name", group->name) == NULL ||
        cJSON_AddStringToObject(obj, "visibility", group->visibility) == NULL ||
        cJSON_AddStringToObject(obj, "owner_user_id", oid_str) == NULL ||
        cJSON_AddNumberToObject(obj, "member_count", (double)group->member_count) == NULL ||
        cJSON_AddNumberToObject(obj, "created_at", (double)group->created_at) == NULL) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

static cJSON *create_group_member_json(const ProtocolGroupMember *member)
{
    if (member == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    char uid_str[32];
    snprintf(uid_str, sizeof(uid_str), "%lld", (long long)member->user_id);

    if (cJSON_AddStringToObject(obj, "user_id", uid_str) == NULL ||
        cJSON_AddStringToObject(obj, "username", member->username) == NULL ||
        cJSON_AddStringToObject(obj, "role", member->role) == NULL ||
        cJSON_AddNumberToObject(obj, "joined_at", (double)member->joined_at) == NULL) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

static cJSON *create_closed_group_json(const ProtocolClosedGroup *closed_group)
{
    if (closed_group == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    if (obj == NULL) {
        return NULL;
    }
    char gid_str[32], by_id_str[32];
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)closed_group->group_id);
    snprintf(by_id_str, sizeof(by_id_str), "%lld", (long long)closed_group->by_user_id);

    if (cJSON_AddStringToObject(obj, "group_id", gid_str) == NULL ||
        cJSON_AddStringToObject(obj, "group_name", closed_group->group_name) == NULL ||
        cJSON_AddStringToObject(obj, "by_user_id", by_id_str) == NULL ||
        cJSON_AddNumberToObject(obj, "closed_at", (double)closed_group->closed_at) == NULL ||
        cJSON_AddStringToObject(obj, "reason", closed_group->reason[0] != '\0' ? closed_group->reason : "owner_left") == NULL) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

static cJSON *create_group_invitation_json(const ProtocolGroupInvitation *invitation)
{
    if (invitation == NULL) {
        return NULL;
    }
    cJSON *obj = cJSON_CreateObject();
    cJSON *inviter = cJSON_CreateObject();
    cJSON *invitee = cJSON_CreateObject();
    if (obj == NULL || inviter == NULL || invitee == NULL) {
        cJSON_Delete(obj);
        cJSON_Delete(inviter);
        cJSON_Delete(invitee);
        return NULL;
    }
    char inv_id_str[32], gid_str[32], inviter_id_str[32], invitee_id_str[32];
    snprintf(inv_id_str, sizeof(inv_id_str), "%lld", (long long)invitation->invitation_id);
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)invitation->group_id);
    snprintf(inviter_id_str, sizeof(inviter_id_str), "%lld", (long long)invitation->inviter_user_id);
    snprintf(invitee_id_str, sizeof(invitee_id_str), "%lld", (long long)invitation->invitee_user_id);

    if (cJSON_AddStringToObject(inviter, "user_id", inviter_id_str) == NULL ||
        cJSON_AddStringToObject(inviter, "username", invitation->inviter_username) == NULL ||
        cJSON_AddStringToObject(invitee, "user_id", invitee_id_str) == NULL ||
        cJSON_AddStringToObject(invitee, "username", invitation->invitee_username) == NULL ||
        cJSON_AddStringToObject(obj, "invitation_id", inv_id_str) == NULL ||
        cJSON_AddStringToObject(obj, "group_id", gid_str) == NULL ||
        cJSON_AddStringToObject(obj, "group_name", invitation->group_name) == NULL ||
        !cJSON_AddItemToObject(obj, "inviter", inviter) ||
        !cJSON_AddItemToObject(obj, "invitee", invitee) ||
        cJSON_AddStringToObject(obj, "status", invitation->status[0] != '\0' ? invitation->status : "pending") == NULL ||
        cJSON_AddNumberToObject(obj, "created_at", (double)invitation->created_at) == NULL) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

ProtocolResult protocol_encode_group_response(const ProtocolRequest *request,
                                              const ProtocolGroup *group,
                                              ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        request->request_id == NULL || group == NULL ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *group_obj = create_group_json(group);
    if (group_obj == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (!cJSON_AddItemToObject(data, "group", group_obj)) {
        cJSON_Delete(group_obj);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_list_response(const ProtocolRequest *request,
                                                   const ProtocolGroup *groups,
                                                   size_t count,
                                                   bool has_more,
                                                   int64_t next_after_id,
                                                   ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.list") != 0 || request->request_id == NULL ||
        (count > 0 && groups == NULL) || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *items = cJSON_CreateArray();
    if (items == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = create_group_json(&groups[i]);
        if (item == NULL) {
            cJSON_Delete(items);
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
        cJSON_AddItemToArray(items, item);
    }
    if (!cJSON_AddItemToObject(data, "items", items) ||
        cJSON_AddBoolToObject(data, "has_more", has_more) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (has_more && count > 0 && next_after_id > 0) {
        char next_str[32];
        snprintf(next_str, sizeof(next_str), "%lld", (long long)next_after_id);
        if (cJSON_AddStringToObject(data, "next_after_id", next_str) == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    } else {
        if (cJSON_AddNullToObject(data, "next_after_id") == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_closed_list_response(const ProtocolRequest *request,
                                                          const ProtocolClosedGroup *groups,
                                                          size_t count,
                                                          bool has_more,
                                                          int64_t next_after_id,
                                                          ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.closed.list") != 0 || request->request_id == NULL ||
        (count > 0 && groups == NULL) || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *items = cJSON_CreateArray();
    if (items == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = create_closed_group_json(&groups[i]);
        if (item == NULL) {
            cJSON_Delete(items);
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
        cJSON_AddItemToArray(items, item);
    }
    if (!cJSON_AddItemToObject(data, "items", items) ||
        cJSON_AddBoolToObject(data, "has_more", has_more) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (has_more && count > 0 && next_after_id > 0) {
        char next_str[32];
        snprintf(next_str, sizeof(next_str), "%lld", (long long)next_after_id);
        if (cJSON_AddStringToObject(data, "next_after_id", next_str) == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    } else {
        if (cJSON_AddNullToObject(data, "next_after_id") == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_members_response(const ProtocolRequest *request,
                                                      const ProtocolGroupMember *members,
                                                      size_t count,
                                                      bool has_more,
                                                      int64_t next_after_id,
                                                      ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.members") != 0 || request->request_id == NULL ||
        (count > 0 && members == NULL) || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *items = cJSON_CreateArray();
    if (items == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = create_group_member_json(&members[i]);
        if (item == NULL) {
            cJSON_Delete(items);
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
        cJSON_AddItemToArray(items, item);
    }
    if (!cJSON_AddItemToObject(data, "items", items) ||
        cJSON_AddBoolToObject(data, "has_more", has_more) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (has_more && count > 0 && next_after_id > 0) {
        char next_str[32];
        snprintf(next_str, sizeof(next_str), "%lld", (long long)next_after_id);
        if (cJSON_AddStringToObject(data, "next_after_id", next_str) == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    } else {
        if (cJSON_AddNullToObject(data, "next_after_id") == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_invite_send_response(const ProtocolRequest *request,
                                                          const ProtocolGroupInvitation *invitation,
                                                          ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.invite.send") != 0 || request->request_id == NULL ||
        invitation == NULL || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *inv_obj = create_group_invitation_json(invitation);
    if (inv_obj == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (!cJSON_AddItemToObject(data, "invitation", inv_obj)) {
        cJSON_Delete(inv_obj);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_invite_list_response(const ProtocolRequest *request,
                                                          const ProtocolGroupInvitation *invitations,
                                                          size_t count,
                                                          bool has_more,
                                                          int64_t next_after_id,
                                                          ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.invite.list") != 0 || request->request_id == NULL ||
        (count > 0 && invitations == NULL) || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *items = cJSON_CreateArray();
    if (items == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = create_group_invitation_json(&invitations[i]);
        if (item == NULL) {
            cJSON_Delete(items);
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
        cJSON_AddItemToArray(items, item);
    }
    if (!cJSON_AddItemToObject(data, "items", items) ||
        cJSON_AddBoolToObject(data, "has_more", has_more) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (has_more && count > 0 && next_after_id > 0) {
        char next_str[32];
        snprintf(next_str, sizeof(next_str), "%lld", (long long)next_after_id);
        if (cJSON_AddStringToObject(data, "next_after_id", next_str) == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    } else {
        if (cJSON_AddNullToObject(data, "next_after_id") == NULL) {
            cJSON_Delete(data);
            return PROTOCOL_ALLOCATION_ERROR;
        }
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_invite_reject_response(const ProtocolRequest *request,
                                                            int64_t invitation_id,
                                                            ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.invite.reject") != 0 || request->request_id == NULL ||
        invitation_id <= 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%lld", (long long)invitation_id);
    if (cJSON_AddStringToObject(data, "invitation_id", id_str) == NULL ||
        cJSON_AddStringToObject(data, "status", "rejected") == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_member_remove_response(const ProtocolRequest *request,
                                                            int64_t group_id,
                                                            int64_t member_user_id,
                                                            ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.member.remove") != 0 || request->request_id == NULL ||
        group_id <= 0 || member_user_id <= 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char gid_str[32], uid_str[32];
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)group_id);
    snprintf(uid_str, sizeof(uid_str), "%lld", (long long)member_user_id);
    if (cJSON_AddStringToObject(data, "group_id", gid_str) == NULL ||
        cJSON_AddStringToObject(data, "member_user_id", uid_str) == NULL ||
        cJSON_AddBoolToObject(data, "removed", 1) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_leave_response(const ProtocolRequest *request,
                                                    int64_t group_id,
                                                    bool closed,
                                                    ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.leave") != 0 || request->request_id == NULL ||
        group_id <= 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char gid_str[32];
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)group_id);
    if (cJSON_AddStringToObject(data, "group_id", gid_str) == NULL ||
        cJSON_AddBoolToObject(data, "closed", closed ? 1 : 0) == NULL) {
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    return encode_success_response(request, data, output);
}

ProtocolResult protocol_encode_group_invite_received_event(const ProtocolGroupInvitation *invitation,
                                                           ByteBuffer *output)
{
    if (invitation == NULL || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    cJSON *inv_obj = create_group_invitation_json(invitation);
    if (inv_obj == NULL) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    if (!cJSON_AddItemToObject(data, "invitation", inv_obj) ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "group.invite.received") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_encode_group_invite_resolved_event(int64_t invitation_id,
                                                           int64_t group_id,
                                                           const char *status,
                                                           int64_t by_user_id,
                                                           ByteBuffer *output)
{
    if (invitation_id <= 0 || group_id <= 0 || status == NULL || by_user_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char inv_id_str[32], gid_str[32], by_id_str[32];
    snprintf(inv_id_str, sizeof(inv_id_str), "%lld", (long long)invitation_id);
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)group_id);
    snprintf(by_id_str, sizeof(by_id_str), "%lld", (long long)by_user_id);

    if (cJSON_AddStringToObject(data, "invitation_id", inv_id_str) == NULL ||
        cJSON_AddStringToObject(data, "group_id", gid_str) == NULL ||
        cJSON_AddStringToObject(data, "status", status) == NULL ||
        cJSON_AddStringToObject(data, "by_user_id", by_id_str) == NULL ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "group.invite.resolved") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_encode_group_member_changed_event(int64_t group_id,
                                                          int64_t user_id,
                                                          const char *username,
                                                          const char *action,
                                                          int64_t by_user_id,
                                                          int32_t member_count,
                                                          ByteBuffer *output)
{
    if (group_id <= 0 || user_id <= 0 || username == NULL || action == NULL || by_user_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *user_obj = cJSON_CreateObject();
    if (root == NULL || data == NULL || user_obj == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(user_obj);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char gid_str[32], uid_str[32], by_id_str[32];
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)group_id);
    snprintf(uid_str, sizeof(uid_str), "%lld", (long long)user_id);
    snprintf(by_id_str, sizeof(by_id_str), "%lld", (long long)by_user_id);

    if (cJSON_AddStringToObject(user_obj, "user_id", uid_str) == NULL ||
        cJSON_AddStringToObject(user_obj, "username", username) == NULL ||
        cJSON_AddStringToObject(data, "group_id", gid_str) == NULL ||
        !cJSON_AddItemToObject(data, "user", user_obj) ||
        cJSON_AddStringToObject(data, "action", action) == NULL ||
        cJSON_AddStringToObject(data, "by_user_id", by_id_str) == NULL ||
        cJSON_AddNumberToObject(data, "member_count", (double)member_count) == NULL ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "group.member.changed") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_encode_group_owner_changed_event(int64_t group_id,
                                                         int64_t previous_owner_user_id,
                                                         int64_t new_owner_user_id,
                                                         int64_t at_ms,
                                                         ByteBuffer *output)
{
    if (group_id <= 0 || previous_owner_user_id <= 0 || new_owner_user_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char gid_str[32], prev_id_str[32], new_id_str[32];
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)group_id);
    snprintf(prev_id_str, sizeof(prev_id_str), "%lld", (long long)previous_owner_user_id);
    snprintf(new_id_str, sizeof(new_id_str), "%lld", (long long)new_owner_user_id);

    if (cJSON_AddStringToObject(data, "group_id", gid_str) == NULL ||
        cJSON_AddStringToObject(data, "previous_owner_user_id", prev_id_str) == NULL ||
        cJSON_AddStringToObject(data, "new_owner_user_id", new_id_str) == NULL ||
        cJSON_AddNumberToObject(data, "at", (double)at_ms) == NULL ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "group.owner.changed") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_encode_group_closed_event(int64_t group_id,
                                                  const char *group_name,
                                                  int64_t by_user_id,
                                                  int64_t at_ms,
                                                  ByteBuffer *output)
{
    if (group_id <= 0 || group_name == NULL || by_user_id <= 0 ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char gid_str[32], by_id_str[32];
    snprintf(gid_str, sizeof(gid_str), "%lld", (long long)group_id);
    snprintf(by_id_str, sizeof(by_id_str), "%lld", (long long)by_user_id);

    if (cJSON_AddStringToObject(data, "group_id", gid_str) == NULL ||
        cJSON_AddStringToObject(data, "group_name", group_name) == NULL ||
        cJSON_AddStringToObject(data, "by_user_id", by_id_str) == NULL ||
        cJSON_AddNumberToObject(data, "at", (double)at_ms) == NULL ||
        cJSON_AddStringToObject(data, "reason", "owner_left") == NULL ||
        cJSON_AddNumberToObject(root, "v", 1.0) == NULL ||
        cJSON_AddStringToObject(root, "kind", "event") == NULL ||
        cJSON_AddStringToObject(root, "type", "group.closed") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}

ProtocolResult protocol_encode_group_send_response(const ProtocolRequest *request,
                                                   const char *scope,
                                                   int64_t message_id,
                                                   const char *client_msg_id,
                                                   int64_t created_at,
                                                   const char *status,
                                                   ByteBuffer *output)
{
    if (request == NULL || request->document == NULL || request->type == NULL ||
        strcmp(request->type, "group.send") != 0 || request->request_id == NULL ||
        scope == NULL || message_id <= 0 || client_msg_id == NULL || created_at < 0 ||
        status == NULL || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char msg_id_str[32];
    snprintf(msg_id_str, sizeof(msg_id_str), "%lld", (long long)message_id);

    bool complete = cJSON_AddNumberToObject(root, "v", 1.0) != NULL &&
                    cJSON_AddStringToObject(root, "kind", "response") != NULL &&
                    cJSON_AddStringToObject(root, "type", "group.send") != NULL &&
                    cJSON_AddStringToObject(root, "request_id", request->request_id) != NULL &&
                    cJSON_AddBoolToObject(root, "ok", true) != NULL &&
                    cJSON_AddStringToObject(data, "scope", scope) != NULL &&
                    cJSON_AddStringToObject(data, "message_id", msg_id_str) != NULL &&
                    cJSON_AddStringToObject(data, "client_msg_id", client_msg_id) != NULL &&
                    cJSON_AddNumberToObject(data, "created_at", (double)created_at) != NULL &&
                    cJSON_AddStringToObject(data, "status", status) != NULL;

    if (complete) {
        complete = cJSON_AddItemToObject(root, "data", data);
        if (complete) {
            data = NULL;
        }
    }

    cJSON_Delete(data);

    if (!complete) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }

    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}

ProtocolResult protocol_encode_group_message_event(int64_t message_id,
                                                   const char *client_msg_id,
                                                   int64_t group_id,
                                                   const char *group_name,
                                                   int64_t sender_id,
                                                   const char *sender_username,
                                                   const char *text,
                                                   int64_t created_at,
                                                   ByteBuffer *output)
{
    if (message_id <= 0 || client_msg_id == NULL || group_id <= 0 ||
        group_name == NULL || sender_id <= 0 || sender_username == NULL ||
        text == NULL || created_at < 0 || output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *sender = cJSON_CreateObject();
    if (root == NULL || data == NULL || sender == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(data);
        cJSON_Delete(sender);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char msg_id_str[32];
    snprintf(msg_id_str, sizeof(msg_id_str), "%lld", (long long)message_id);

    char group_id_str[32];
    snprintf(group_id_str, sizeof(group_id_str), "%lld", (long long)group_id);

    char sender_id_str[32];
    snprintf(sender_id_str, sizeof(sender_id_str), "%lld", (long long)sender_id);

    bool complete = cJSON_AddStringToObject(sender, "user_id", sender_id_str) != NULL &&
                    cJSON_AddStringToObject(sender, "username", sender_username) != NULL;

    if (complete) {
        complete = cJSON_AddItemToObject(data, "sender", sender);
        if (complete) {
            sender = NULL;
        }
    }

    cJSON_Delete(sender);

    if (complete) {
        complete = cJSON_AddStringToObject(data, "message_id", msg_id_str) != NULL &&
                   cJSON_AddStringToObject(data, "client_msg_id", client_msg_id) != NULL &&
                   cJSON_AddStringToObject(data, "group_id", group_id_str) != NULL &&
                   cJSON_AddStringToObject(data, "group_name", group_name) != NULL &&
                   cJSON_AddStringToObject(data, "text", text) != NULL &&
                   cJSON_AddNumberToObject(data, "created_at", (double)created_at) != NULL &&
                   cJSON_AddNumberToObject(root, "v", 1.0) != NULL &&
                   cJSON_AddStringToObject(root, "kind", "event") != NULL &&
                   cJSON_AddStringToObject(root, "type", "group.message") != NULL &&
                   cJSON_AddItemToObject(root, "data", data);
        if (complete) {
            data = NULL;
        }
    }

    cJSON_Delete(data);

    if (!complete) {
        cJSON_Delete(root);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }

    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}


static const char *error_code_name(ProtocolErrorCode code)
{
    switch (code) {
    case PROTOCOL_ERROR_INVALID_JSON:
        return "INVALID_JSON";
    case PROTOCOL_ERROR_INVALID_REQUEST:
        return "INVALID_REQUEST";
    case PROTOCOL_ERROR_UNSUPPORTED_VERSION:
        return "UNSUPPORTED_VERSION";
    case PROTOCOL_ERROR_UNKNOWN_TYPE:
        return "UNKNOWN_TYPE";
    case PROTOCOL_ERROR_VALIDATION:
        return "VALIDATION_ERROR";
    case PROTOCOL_ERROR_REQUEST_ID_REUSED:
        return "REQUEST_ID_REUSED";
    case PROTOCOL_ERROR_REQUEST_ID_EXPIRED:
        return "REQUEST_ID_EXPIRED";
    case PROTOCOL_ERROR_USERNAME_TAKEN:
        return "USERNAME_TAKEN";
    case PROTOCOL_ERROR_STORAGE:
        return "STORAGE_ERROR";
    case PROTOCOL_ERROR_INTERNAL:
        return "INTERNAL_ERROR";
    case PROTOCOL_ERROR_AUTH_REQUIRED:
        return "AUTH_REQUIRED";
    case PROTOCOL_ERROR_ALREADY_AUTHENTICATED:
        return "ALREADY_AUTHENTICATED";
    case PROTOCOL_ERROR_ALREADY_ONLINE:
        return "ALREADY_ONLINE";
    case PROTOCOL_ERROR_AUTH_FAILED:
        return "AUTH_FAILED";
    case PROTOCOL_ERROR_NOT_FOUND:
        return "NOT_FOUND";
    case PROTOCOL_ERROR_CONFLICT:
        return "CONFLICT";
    case PROTOCOL_ERROR_FORBIDDEN:
        return "FORBIDDEN";
    case PROTOCOL_ERROR_IDEMPOTENCY_CONFLICT:
        return "IDEMPOTENCY_CONFLICT";
    case PROTOCOL_ERROR_GROUP_CLOSED:
        return "GROUP_CLOSED";
    case PROTOCOL_ERROR_LIMIT_EXCEEDED:
        return "LIMIT_EXCEEDED";
    }
    return NULL;
}

static const char *error_message(ProtocolErrorCode code)
{
    switch (code) {
    case PROTOCOL_ERROR_INVALID_JSON:
        return "JSON không hợp lệ.";
    case PROTOCOL_ERROR_INVALID_REQUEST:
        return "Request không hợp lệ.";
    case PROTOCOL_ERROR_UNSUPPORTED_VERSION:
        return "Phiên bản giao thức không được hỗ trợ.";
    case PROTOCOL_ERROR_UNKNOWN_TYPE:
        return "Loại request không được hỗ trợ.";
    case PROTOCOL_ERROR_VALIDATION:
        return "Dữ liệu request không hợp lệ.";
    case PROTOCOL_ERROR_REQUEST_ID_REUSED:
        return "Request ID đã được sử dụng với nội dung khác.";
    case PROTOCOL_ERROR_REQUEST_ID_EXPIRED:
        return "Request ID không lớn hơn request trước đó hoặc đã hết hạn.";
    case PROTOCOL_ERROR_USERNAME_TAKEN:
        return "Tên người dùng đã được sử dụng.";
    case PROTOCOL_ERROR_STORAGE:
        return "Lỗi cơ sở dữ liệu hoặc lưu trữ.";
    case PROTOCOL_ERROR_INTERNAL:
        return "Lỗi máy chủ nội bộ.";
    case PROTOCOL_ERROR_AUTH_REQUIRED:
        return "Yêu cầu đăng nhập để thực hiện thao tác này.";
    case PROTOCOL_ERROR_ALREADY_AUTHENTICATED:
        return "Kết nối đã được xác thực trước đó.";
    case PROTOCOL_ERROR_ALREADY_ONLINE:
        return "Tài khoản hiện đang online trên một kết nối khác.";
    case PROTOCOL_ERROR_AUTH_FAILED:
        return "Tên đăng nhập hoặc mật khẩu không chính xác.";
    case PROTOCOL_ERROR_NOT_FOUND:
        return "Không tìm thấy đối tượng yêu cầu.";
    case PROTOCOL_ERROR_CONFLICT:
        return "Xung đột trạng thái hoặc dữ liệu đã tồn tại.";
    case PROTOCOL_ERROR_FORBIDDEN:
        return "Thao tác bị từ chối do không đủ quyền.";
    case PROTOCOL_ERROR_IDEMPOTENCY_CONFLICT:
        return "Cùng client_msg_id nhưng khác đích hoặc nội dung.";
    case PROTOCOL_ERROR_GROUP_CLOSED:
        return "Nhóm đã bị giải tán hoặc đóng.";
    case PROTOCOL_ERROR_LIMIT_EXCEEDED:
        return "Đã đạt giới hạn tối đa cho phép.";
    }
    return NULL;
}

static ProtocolResult encode_error(ProtocolErrorCode code, const ProtocolRequest *request,
                                   ByteBuffer *output)
{
    const char *name = error_code_name(code);
    const char *message = error_message(code);
    if (name == NULL || message == NULL || output == NULL || output->data == NULL ||
        (request != NULL && (request->document == NULL || request->type == NULL ||
                             request->request_id == NULL))) {
        return PROTOCOL_INVALID_REQUEST;
    }

    cJSON *envelope = cJSON_CreateObject();
    cJSON *details = cJSON_CreateObject();
    if (envelope == NULL || details == NULL) {
        cJSON_Delete(envelope);
        cJSON_Delete(details);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    bool complete = cJSON_AddNumberToObject(envelope, "v", 1) != NULL &&
                    cJSON_AddStringToObject(envelope, "kind",
                                            request == NULL ? "event" : "response") != NULL &&
                    cJSON_AddStringToObject(envelope, "type",
                                            request == NULL ? "protocol.error" : request->type) != NULL;
    if (complete && request != NULL) {
        complete = cJSON_AddStringToObject(envelope, "request_id",
                                           request->request_id) != NULL &&
                   cJSON_AddBoolToObject(envelope, "ok", false) != NULL;
    }
    complete = complete && cJSON_AddStringToObject(details, "code", name) != NULL &&
               cJSON_AddStringToObject(details, "message", message) != NULL;
    if (complete && request != NULL) {
        complete = cJSON_AddBoolToObject(details, "retryable", false) != NULL;
    }
    if (complete) {
        complete = cJSON_AddItemToObject(envelope, request == NULL ? "data" : "error",
                                         details);
        if (complete) {
            details = NULL;
        }
    }
    cJSON_Delete(details);
    if (!complete) {
        cJSON_Delete(envelope);
        return PROTOCOL_ALLOCATION_ERROR;
    }

    char *json = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_encode_error_response(const ProtocolRequest *request,
                                               ProtocolErrorCode code, ByteBuffer *output)
{
    return encode_error(code, request, output);
}

ProtocolResult protocol_encode_protocol_error(ProtocolErrorCode code, ByteBuffer *output)
{
    return encode_error(code, NULL, output);
}

ProtocolResult protocol_encode_ready_event(int64_t server_time_ms, ByteBuffer *output)
{
    if (server_time_ms < 0 || server_time_ms > 9007199254740991LL ||
        output == NULL || output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *event = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (event == NULL || data == NULL) {
        cJSON_Delete(event);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    bool complete = cJSON_AddNumberToObject(event, "v", 1) != NULL &&
                    cJSON_AddStringToObject(event, "kind", "event") != NULL &&
                    cJSON_AddStringToObject(event, "type", "connection.ready") != NULL &&
                    cJSON_AddNumberToObject(data, "protocol_version", 1) != NULL &&
                    cJSON_AddNumberToObject(data, "max_frame_bytes",
                                            CHAT_MAX_FRAME_BODY_BYTES) != NULL &&
                    cJSON_AddNumberToObject(data, "server_time", (double)server_time_ms) != NULL;
    if (complete) {
        complete = cJSON_AddItemToObject(event, "data", data);
        if (complete) {
            data = NULL;
        }
    }
    cJSON_Delete(data);
    if (!complete) {
        cJSON_Delete(event);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_validate_ready_event(const unsigned char *body, size_t length)
{
    cJSON *document = NULL;
    ProtocolResult result = parse_document(body, length, &document);
    if (result != PROTOCOL_OK) {
        return result;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(document, "v");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(document, "kind");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(document, "type");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(document, "data");
    const cJSON *protocol_version = cJSON_GetObjectItemCaseSensitive(data, "protocol_version");
    const cJSON *max_frame_bytes = cJSON_GetObjectItemCaseSensitive(data, "max_frame_bytes");
    const cJSON *server_time = cJSON_GetObjectItemCaseSensitive(data, "server_time");
    /* Event đầu tiên phải đúng shape; client không gửi request nếu version lệch. */
    if (!cJSON_IsObject(document) || cJSON_GetArraySize(document) != 4 ||
        !cJSON_IsNumber(version) || version->valuedouble != 1 ||
        !cJSON_IsString(kind) || strcmp(kind->valuestring, "event") != 0 ||
        !cJSON_IsString(type) || strcmp(type->valuestring, "connection.ready") != 0 ||
        !cJSON_IsObject(data) || cJSON_GetArraySize(data) != 3 ||
        !cJSON_IsNumber(protocol_version) || protocol_version->valuedouble != 1 ||
        !cJSON_IsNumber(max_frame_bytes) ||
        max_frame_bytes->valuedouble != CHAT_MAX_FRAME_BODY_BYTES ||
        !cJSON_IsNumber(server_time) || server_time->valuedouble < 0 ||
        server_time->valuedouble > 9007199254740991.0 ||
        (double)(uint64_t)server_time->valuedouble != server_time->valuedouble) {
        result = PROTOCOL_INVALID_REQUEST;
    }
    cJSON_Delete(document);
    return result;
}

bool protocol_validate_nonce_hex(const char *nonce)
{
    if (nonce == NULL) {
        return false;
    }
    for (size_t index = 0; index < CHAT_HEARTBEAT_NONCE_HEX_LEN; ++index) {
        char ch = nonce[index];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return nonce[CHAT_HEARTBEAT_NONCE_HEX_LEN] == '\0';
}

ProtocolResult protocol_encode_heartbeat_ping(const char *nonce, ByteBuffer *output)
{
    if (nonce == NULL || !protocol_validate_nonce_hex(nonce) || output == NULL ||
        output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *event = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (event == NULL || data == NULL) {
        cJSON_Delete(event);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    bool complete = cJSON_AddNumberToObject(event, "v", 1) != NULL &&
                    cJSON_AddStringToObject(event, "kind", "event") != NULL &&
                    cJSON_AddStringToObject(event, "type", "heartbeat.ping") != NULL &&
                    cJSON_AddStringToObject(data, "nonce", nonce) != NULL;
    if (complete) {
        complete = cJSON_AddItemToObject(event, "data", data);
        if (complete) {
            data = NULL;
        }
    }
    cJSON_Delete(data);
    if (!complete) {
        cJSON_Delete(event);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



ProtocolResult protocol_parse_heartbeat_pong(const unsigned char *body, size_t length,
                                             char *nonce_out)
{
    if (body == NULL || nonce_out == NULL || length == 0 ||
        length > CHAT_MAX_FRAME_BODY_BYTES) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *document = NULL;
    ProtocolResult result = parse_document(body, length, &document);
    if (result != PROTOCOL_OK) {
        return result;
    }
    if (!cJSON_IsObject(document) || cJSON_GetArraySize(document) != 4) {
        cJSON_Delete(document);
        return PROTOCOL_INVALID_REQUEST;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(document, "v");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(document, "kind");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(document, "type");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(document, "data");

    if (!cJSON_IsNumber(version) || version->valuedouble != 1.0 ||
        !cJSON_IsString(kind) || strcmp(kind->valuestring, "control") != 0 ||
        !cJSON_IsString(type) || strcmp(type->valuestring, "heartbeat.pong") != 0 ||
        !cJSON_IsObject(data) || cJSON_GetArraySize(data) != 1) {
        cJSON_Delete(document);
        return PROTOCOL_INVALID_REQUEST;
    }

    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(data, "nonce");
    if (!cJSON_IsString(nonce) || !protocol_validate_nonce_hex(nonce->valuestring)) {
        cJSON_Delete(document);
        return PROTOCOL_INVALID_REQUEST;
    }

    memcpy(nonce_out, nonce->valuestring, CHAT_HEARTBEAT_NONCE_HEX_LEN);
    nonce_out[CHAT_HEARTBEAT_NONCE_HEX_LEN] = '\0';
    cJSON_Delete(document);
    return PROTOCOL_OK;
}

bool protocol_generate_nonce_hex(char *nonce_out, size_t size)
{
    if (nonce_out == NULL || size < CHAT_HEARTBEAT_NONCE_HEX_LEN + 1) {
        return false;
    }
    unsigned char bytes[16];
    ssize_t ret = getrandom(bytes, sizeof(bytes), GRND_NONBLOCK);
    if (ret != (ssize_t)sizeof(bytes)) {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t read_bytes = read(fd, bytes, sizeof(bytes));
            close(fd);
            if (read_bytes == (ssize_t)sizeof(bytes)) {
                ret = (ssize_t)sizeof(bytes);
            }
        }
    }
    if (ret != (ssize_t)sizeof(bytes)) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t seed1 = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
        static uint64_t counter = 0;
        uint64_t seed2 = ++counter * 6364136223846793005ULL + 1442695040888963407ULL;
        memcpy(bytes, &seed1, 8);
        memcpy(bytes + 8, &seed2, 8);
    }
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        snprintf(nonce_out + i * 2, 3, "%02x", bytes[i]);
    }
    nonce_out[CHAT_HEARTBEAT_NONCE_HEX_LEN] = '\0';
    return true;
}

ProtocolResult protocol_parse_heartbeat_ping(const unsigned char *body, size_t length,
                                             char *nonce_out)
{
    if (body == NULL || nonce_out == NULL || length == 0 ||
        length > CHAT_MAX_FRAME_BODY_BYTES) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *document = NULL;
    ProtocolResult result = parse_document(body, length, &document);
    if (result != PROTOCOL_OK) {
        return result;
    }
    if (!cJSON_IsObject(document) || cJSON_GetArraySize(document) != 4) {
        cJSON_Delete(document);
        return PROTOCOL_INVALID_REQUEST;
    }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(document, "v");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(document, "kind");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(document, "type");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(document, "data");

    if (!cJSON_IsNumber(version) || version->valuedouble != 1.0 ||
        !cJSON_IsString(kind) || strcmp(kind->valuestring, "event") != 0 ||
        !cJSON_IsString(type) || strcmp(type->valuestring, "heartbeat.ping") != 0 ||
        !cJSON_IsObject(data) || cJSON_GetArraySize(data) != 1) {
        cJSON_Delete(document);
        return PROTOCOL_INVALID_REQUEST;
    }

    const cJSON *nonce = cJSON_GetObjectItemCaseSensitive(data, "nonce");
    if (!cJSON_IsString(nonce) || !protocol_validate_nonce_hex(nonce->valuestring)) {
        cJSON_Delete(document);
        return PROTOCOL_INVALID_REQUEST;
    }

    memcpy(nonce_out, nonce->valuestring, CHAT_HEARTBEAT_NONCE_HEX_LEN);
    nonce_out[CHAT_HEARTBEAT_NONCE_HEX_LEN] = '\0';
    cJSON_Delete(document);
    return PROTOCOL_OK;
}

ProtocolResult protocol_encode_heartbeat_pong(const char *nonce, ByteBuffer *output)
{
    if (nonce == NULL || !protocol_validate_nonce_hex(nonce) || output == NULL ||
        output->data == NULL) {
        return PROTOCOL_INVALID_REQUEST;
    }
    cJSON *control = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (control == NULL || data == NULL) {
        cJSON_Delete(control);
        cJSON_Delete(data);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    bool complete = cJSON_AddNumberToObject(control, "v", 1) != NULL &&
                    cJSON_AddStringToObject(control, "kind", "control") != NULL &&
                    cJSON_AddStringToObject(control, "type", "heartbeat.pong") != NULL &&
                    cJSON_AddStringToObject(data, "nonce", nonce) != NULL;
    if (complete) {
        complete = cJSON_AddItemToObject(control, "data", data);
        if (complete) {
            data = NULL;
        }
    }
    cJSON_Delete(data);
    if (!complete) {
        cJSON_Delete(control);
        return PROTOCOL_ALLOCATION_ERROR;
    }
    char *json = cJSON_PrintUnformatted(control);
    cJSON_Delete(control);
    if (json == NULL) {
        return PROTOCOL_ALLOCATION_ERROR;
    }
    FrameResult framed = frame_encode(output, json, strlen(json));
    cJSON_free(json);
    return framed == FRAME_OK ? PROTOCOL_OK : PROTOCOL_OUTPUT_FULL;
}



