#define _POSIX_C_SOURCE 200809L

#include "server/net/connection.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "server/core/router.h"
#include "server/repos/activity_repo.h"
#include "shared/frame.h"
#include "shared/limits.h"
#include "shared/protocol.h"

static uint64_t global_connection_generation = 0;

static ConnectionReceiveResult router_to_receive_result(RouterResult result)
{
    switch (result) {
    case ROUTER_OK:
    case ROUTER_CLOSE_AFTER_FLUSH:
        return CONNECTION_RECEIVE_OK;
    case ROUTER_OUTPUT_FULL:
        return CONNECTION_RECEIVE_OUTPUT_FULL;
    case ROUTER_INTERNAL_ERROR:
        return CONNECTION_RECEIVE_IO_ERROR;
    }
    return CONNECTION_RECEIVE_IO_ERROR;
}

static bool monotonic_milliseconds(int64_t *milliseconds)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return false;
    }
    *milliseconds = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return true;
}

static ConnectionReceiveResult process_frames(Connection *connection, size_t *frames_processed)
{
    FrameView view = {0};
    FrameResult frame_result;

    while (*frames_processed < CHAT_FRAMES_PER_CONNECTION_TURN) {
        frame_result = frame_peek(&connection->input, &view);
        if (frame_result == FRAME_NEED_MORE) {
            connection->pending_frame = false;
            return CONNECTION_RECEIVE_OK;
        }
        if (frame_result != FRAME_OK) {
            return CONNECTION_RECEIVE_INVALID_FRAME;
        }

        /* Bỏ byte đã gửi khỏi đầu queue trước khi nối response mới. */
        if (connection->output_offset != 0) {
            if (buffer_consume(&connection->output, connection->output_offset) != BUFFER_OK) {
                return CONNECTION_RECEIVE_OUTPUT_FULL;
            }
            connection->output_offset = 0;
        }

        /* Xử lý control heartbeat.pong trước router; pong không có response hay request ID. */
        char pong_nonce[CHAT_HEARTBEAT_NONCE_HEX_LEN + 1];
        ProtocolResult pong_result = protocol_parse_heartbeat_pong(
            view.body, view.length, pong_nonce);
        if (pong_result == PROTOCOL_OK) {
            if (connection->heartbeat_outstanding &&
                strcmp(connection->heartbeat_nonce, pong_nonce) == 0) {
                connection->heartbeat_outstanding = false;
                connection->heartbeat_nonce[0] = '\0';
                connection->heartbeat_deadline_ms = 0;
                connection->invalid_pong_count = 0;
                monotonic_milliseconds(&connection->last_activity_ms);
            } else {
                ++connection->invalid_pong_count;
                if (connection->invalid_pong_count >= CHAT_MAX_INVALID_PONGS) {
                    int64_t now_ms;
                    if (!monotonic_milliseconds(&now_ms)) {
                        return CONNECTION_RECEIVE_IO_ERROR;
                    }
                    connection->close_after_flush = true;
                    connection->close_deadline_ms = now_ms + CHAT_CLOSE_FLUSH_TIMEOUT_MS;
                }
            }
            if (frame_consume(&connection->input) != FRAME_OK) {
                return CONNECTION_RECEIVE_INVALID_FRAME;
            }
            ++*frames_processed;
            if (connection->close_after_flush) {
                connection->pending_frame = false;
                return CONNECTION_RECEIVE_OK;
            }
            continue;
        }

        bool output_was_empty = connection->output.length == 0;
        char conn_id[32];
        snprintf(conn_id, sizeof(conn_id), "conn-%d", connection->fd);
        /* Router đọc JSON và xếp frame response; networking không biết type nghiệp vụ. */
        RouterResult routed = router_handle_frame(view.body, view.length,
                                                   &connection->output,
                                                   &connection->request_cache,
                                                   &connection->high_water_request_id,
                                                   connection->db,
                                                   connection->session_mgr,
                                                   conn_id,
                                                   connection->generation);
        if (routed != ROUTER_OK && routed != ROUTER_CLOSE_AFTER_FLUSH) {
            return router_to_receive_result(routed);
        }
        if (output_was_empty &&
            !monotonic_milliseconds(&connection->output_last_progress_ms)) {
            return CONNECTION_RECEIVE_IO_ERROR;
        }
        if (frame_consume(&connection->input) != FRAME_OK) {
            return CONNECTION_RECEIVE_INVALID_FRAME;
        }
        monotonic_milliseconds(&connection->last_activity_ms);
        ++*frames_processed;
        if (routed == ROUTER_CLOSE_AFTER_FLUSH) {
            /* Không đọc thêm frame; xả response cuối và các response trước nó theo thứ tự. */
            int64_t now_ms;
            if (!monotonic_milliseconds(&now_ms)) {
                return CONNECTION_RECEIVE_IO_ERROR;
            }
            connection->close_after_flush = true;
            connection->close_deadline_ms = now_ms + CHAT_CLOSE_FLUSH_TIMEOUT_MS;
            connection->pending_frame = false;
            return CONNECTION_RECEIVE_OK;
        }
    }

    /* Frame thứ 17 vẫn ở buffer; main sẽ chạy lượt mới mà không chờ POLLIN. */
    frame_result = frame_peek(&connection->input, &view);
    if (frame_result == FRAME_INVALID_LENGTH || frame_result == FRAME_INVALID_ARGUMENT) {
        return CONNECTION_RECEIVE_INVALID_FRAME;
    }
    connection->pending_frame = frame_result == FRAME_OK;
    return CONNECTION_RECEIVE_OK;
}

BufferResult connection_init(Connection *connection, int fd)
{
    if (connection == NULL || fd < 0 || connection->input.data != NULL ||
        connection->input.length != 0 || connection->input.capacity != 0) {
        return BUFFER_INVALID_ARGUMENT;
    }

    BufferResult result = buffer_init(&connection->input, CHAT_INPUT_BUFFER_CAPACITY);
    if (result != BUFFER_OK) {
        return result;
    }
    result = buffer_init(&connection->output, CHAT_OUTPUT_BUFFER_CAPACITY);
    if (result != BUFFER_OK) {
        buffer_destroy(&connection->input);
        return result;
    }

    connection->fd = fd;
    connection->output_offset = 0;
    connection->output_last_progress_ms = 0;
    connection->close_deadline_ms = 0;
    connection->pending_frame = false;
    connection->peer_eof = false;
    connection->close_after_flush = false;
    connection->ready_queued = false;
    connection->heartbeat_outstanding = false;
    connection->heartbeat_nonce[0] = '\0';
    connection->heartbeat_deadline_ms = 0;
    connection->invalid_pong_count = 0;
    connection->high_water_request_id = 0;
    connection->delivery_cursor_message_id = 0;
    connection->delivery_cursor_group_message_id = 0;
    connection->delivery_scope_turn = false;
    connection->delivery_fatal_timeout = false;
    connection->db = NULL;
    connection->session_mgr = NULL;
    connection->generation = ++global_connection_generation;
    request_cache_init(&connection->request_cache);
    delivery_tracker_init(&connection->delivery_tracker);
    monotonic_milliseconds(&connection->last_activity_ms);
    return BUFFER_OK;
}

void connection_set_db(Connection *connection, Database *db)
{
    if (connection != NULL) {
        connection->db = db;
    }
}

void connection_set_session_manager(Connection *connection, SessionManager *session_mgr)
{
    if (connection != NULL) {
        connection->session_mgr = session_mgr;
    }
}

DeliveryTracker *connection_get_delivery_tracker(Connection *connection)
{
    if (connection == NULL) {
        return NULL;
    }
    return &connection->delivery_tracker;
}

uint64_t connection_get_generation(const Connection *connection)
{
    if (connection == NULL) {
        return 0;
    }
    return connection->generation;
}

bool connection_queue_ready(Connection *connection)
{
    if (connection == NULL || connection->fd < 0 || connection->output.data == NULL ||
        connection->output.length != 0 || connection->output_offset != 0 ||
        connection->output_last_progress_ms != 0 || connection->close_after_flush ||
        connection->ready_queued) {
        return false;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == -1) {
        return false;
    }
    int64_t server_time_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    /* Event được queue trước khi main nhận request; thứ tự output luôn ready → response. */
    if (protocol_encode_ready_event(server_time_ms, &connection->output) != PROTOCOL_OK) {
        return false;
    }
    if (!monotonic_milliseconds(&connection->output_last_progress_ms)) {
        return false;
    }
    connection->ready_queued = true;
    return true;
}

bool connection_queue_heartbeat_ping(Connection *connection, const char *nonce,
                                     int64_t deadline_ms)
{
    if (connection == NULL || connection->fd < 0 || connection->output.data == NULL ||
        connection->close_after_flush || connection->heartbeat_outstanding ||
        nonce == NULL || deadline_ms <= 0) {
        return false;
    }
    bool output_was_empty = connection->output.length == 0;
    if (protocol_encode_heartbeat_ping(nonce, &connection->output) != PROTOCOL_OK) {
        return false;
    }
    if (output_was_empty &&
        !monotonic_milliseconds(&connection->output_last_progress_ms)) {
        return false;
    }
    memcpy(connection->heartbeat_nonce, nonce, CHAT_HEARTBEAT_NONCE_HEX_LEN);
    connection->heartbeat_nonce[CHAT_HEARTBEAT_NONCE_HEX_LEN] = '\0';
    connection->heartbeat_deadline_ms = deadline_ms;
    connection->heartbeat_outstanding = true;
    return true;
}

ConnectionReceiveResult connection_receive(Connection *connection, size_t *frames_processed)
{
    if (connection == NULL || frames_processed == NULL || connection->fd < 0 ||
        connection->input.data == NULL) {
        return CONNECTION_RECEIVE_IO_ERROR;
    }

    *frames_processed = 0;
    if (connection->peer_eof) {
        return CONNECTION_RECEIVE_EOF;
    }
    if (connection->close_after_flush) {
        return CONNECTION_RECEIVE_OK;
    }
    ConnectionReceiveResult result = process_frames(connection, frames_processed);
    if (result != CONNECTION_RECEIVE_OK ||
        *frames_processed == CHAT_FRAMES_PER_CONNECTION_TURN) {
        return result;
    }

    size_t available = connection->input.capacity - connection->input.length;
    if (available == 0) {
        return CONNECTION_RECEIVE_INPUT_FULL;
    }

    unsigned char chunk[4096];
    size_t chunk_size = available < sizeof(chunk) ? available : sizeof(chunk);
    ssize_t received = recv(connection->fd, chunk, chunk_size, 0);
    if (received == 0) {
        /* EOF chỉ kết thúc chiều nhận; chiều gửi còn dùng để xả response đã xếp. */
        connection->peer_eof = true;
        return CONNECTION_RECEIVE_EOF;
    }
    if (received == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return CONNECTION_RECEIVE_OK;
        }
        return CONNECTION_RECEIVE_IO_ERROR;
    }

    if (buffer_append(&connection->input, chunk, (size_t)received) != BUFFER_OK) {
        return CONNECTION_RECEIVE_INPUT_FULL;
    }
    monotonic_milliseconds(&connection->last_activity_ms);
    return process_frames(connection, frames_processed);
}

bool connection_flush(Connection *connection)
{
    if (connection == NULL || connection->fd < 0 || connection->output.data == NULL ||
        connection->output_offset > connection->output.length) {
        return false;
    }
    if (connection->output_offset == connection->output.length) {
        buffer_consume(&connection->output, connection->output.length);
        connection->output_offset = 0;
        connection->output_last_progress_ms = 0;
        return true;
    }

    size_t remaining = connection->output.length - connection->output_offset;
    size_t send_size = remaining < 65536u ? remaining : 65536u;
    ssize_t sent = send(connection->fd, connection->output.data + connection->output_offset,
                        send_size, MSG_NOSIGNAL);
    if (sent == -1) {
        return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
    }
    if (sent == 0) {
        return false;
    }

    /* Partial write: không bỏ phần đuôi, chỉ tăng offset đã gửi. */
    connection->output_offset += (size_t)sent;
    if (connection->output_offset == connection->output.length) {
        buffer_consume(&connection->output, connection->output.length);
        connection->output_offset = 0;
        connection->output_last_progress_ms = 0;
        int64_t now_ms;
        if (monotonic_milliseconds(&now_ms)) {
            delivery_tracker_mark_flushed(&connection->delivery_tracker, now_ms);
        }
    } else if (!monotonic_milliseconds(&connection->output_last_progress_ms)) {
        return false;
    }
    return true;
}

bool connection_output_timed_out(const Connection *connection)
{
    if (connection == NULL || connection->output.length <= connection->output_offset ||
        connection->output_last_progress_ms == 0) {
        return false;
    }
    int64_t now_ms;
    if (!monotonic_milliseconds(&now_ms)) {
        return true;
    }
    return now_ms - connection->output_last_progress_ms >=
           CHAT_OUTPUT_NO_PROGRESS_TIMEOUT_MS;
}

bool connection_close_timed_out(const Connection *connection)
{
    if (connection == NULL || !connection->close_after_flush ||
        connection->close_deadline_ms == 0) {
        return false;
    }
    int64_t now_ms;
    return !monotonic_milliseconds(&now_ms) || now_ms >= connection->close_deadline_ms;
}

bool connection_heartbeat_timed_out(const Connection *connection)
{
    if (connection == NULL || !connection->heartbeat_outstanding ||
        connection->heartbeat_deadline_ms == 0) {
        return false;
    }
    int64_t now_ms;
    return !monotonic_milliseconds(&now_ms) || now_ms >= connection->heartbeat_deadline_ms;
}

bool connection_needs_heartbeat_ping(const Connection *connection)
{
    if (connection == NULL || connection->fd < 0 || connection->close_after_flush ||
        connection->peer_eof || connection->heartbeat_outstanding ||
        connection->last_activity_ms == 0) {
        return false;
    }
    int64_t now_ms;
    if (!monotonic_milliseconds(&now_ms)) {
        return false;
    }
    return now_ms - connection->last_activity_ms >= CHAT_HEARTBEAT_IDLE_TIMEOUT_MS;
}

void connection_destroy(Connection *connection)
{
    if (connection == NULL) {
        return;
    }

    if (connection->session_mgr != NULL && connection->fd >= 0) {
        char conn_id[32];
        snprintf(conn_id, sizeof(conn_id), "conn-%d", connection->fd);
        int64_t unbind_uid = 0;
        SessionResult unbind_res = session_manager_unbind_by_connection(
            connection->session_mgr, conn_id, connection->generation, &unbind_uid);
        if (unbind_res == SESSION_OK && connection->db != NULL) {
            struct timespec now;
            int64_t now_ms = 0;
            if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
                now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
            }
            char uid_str[32];
            snprintf(uid_str, sizeof(uid_str), "%lld", (long long)unbind_uid);
            activity_repo_log(connection->db, now_ms, ACTIVITY_LEVEL_INFO, &unbind_uid,
                              conn_id, "connection.disconnect", ACTIVITY_OUTCOME_SUCCESS,
                              ACTIVITY_TARGET_CONNECTION, uid_str, NULL, NULL);

            const char *reason = "eof";
            if (connection->heartbeat_outstanding && connection_heartbeat_timed_out(connection)) {
                reason = "timeout";
            } else if (connection->close_after_flush) {
                reason = "quit";
            }
            router_broadcast_presence(connection->db, connection->session_mgr,
                                      unbind_uid, false, reason, now_ms);
        }
        connection->session_mgr = NULL;
    }

    if (connection->input.data != NULL) {
        close(connection->fd);
    }
    buffer_destroy(&connection->input);
    buffer_destroy(&connection->output);
    connection->fd = -1;
    connection->generation = 0;
    connection->output_offset = 0;
    connection->output_last_progress_ms = 0;
    connection->close_deadline_ms = 0;
    connection->last_activity_ms = 0;
    connection->pending_frame = false;
    connection->peer_eof = false;
    connection->close_after_flush = false;
    connection->ready_queued = false;
    connection->heartbeat_outstanding = false;
    connection->heartbeat_nonce[0] = '\0';
    connection->heartbeat_deadline_ms = 0;
    connection->invalid_pong_count = 0;
    connection->high_water_request_id = 0;
    connection->delivery_cursor_message_id = 0;
    connection->delivery_cursor_group_message_id = 0;
    connection->delivery_scope_turn = false;
    connection->delivery_fatal_timeout = false;
    request_cache_destroy(&connection->request_cache);
    delivery_tracker_clear(&connection->delivery_tracker);
}

bool connection_delivery_fatal_timed_out(const Connection *connection)
{
    return connection != NULL && connection->delivery_fatal_timeout;
}

void connection_set_delivery_fatal_timeout(Connection *connection, bool timed_out)
{
    if (connection != NULL) {
        connection->delivery_fatal_timeout = timed_out;
    }
}
