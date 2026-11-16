#define _POSIX_C_SOURCE 200809L

#include "client/transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "shared/frame.h"
#include "shared/limits.h"
#include "shared/protocol.h"

static bool monotonic_milliseconds(int64_t *milliseconds)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return false;
    }
    *milliseconds = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    return true;
}

static ClientTransportResult check_deadline(int64_t deadline_ms)
{
    int64_t now_ms;
    if (!monotonic_milliseconds(&now_ms)) {
        return CLIENT_TRANSPORT_IO_ERROR;
    }
    return now_ms < deadline_ms ? CLIENT_TRANSPORT_OK : CLIENT_TRANSPORT_TIMEOUT;
}

static ClientTransportResult wait_for_socket(int socket_fd, short requested_events,
                                              int64_t deadline_ms)
{
    struct pollfd socket_poll = {.fd = socket_fd, .events = requested_events};
    for (;;) {
        int64_t now_ms;
        if (!monotonic_milliseconds(&now_ms)) {
            return CLIENT_TRANSPORT_IO_ERROR;
        }
        if (now_ms >= deadline_ms) {
            return CLIENT_TRANSPORT_TIMEOUT;
        }
        /* EINTR hoặc một byte mới không được đặt lại đồng hồ 5 giây. */
        int ready_count = poll(&socket_poll, 1, (int)(deadline_ms - now_ms));
        if (ready_count == -1 && errno == EINTR) {
            continue;
        }
        if (ready_count == -1) {
            return CLIENT_TRANSPORT_IO_ERROR;
        }
        if (ready_count == 0) {
            return CLIENT_TRANSPORT_TIMEOUT;
        }
        if (socket_poll.revents & requested_events) {
            return CLIENT_TRANSPORT_OK;
        }
        return CLIENT_TRANSPORT_IO_ERROR;
    }
}

static ClientTransportResult connect_to_server(const char *host, uint16_t port,
                                                int64_t deadline_ms, int *socket_fd)
{
    *socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (*socket_fd == -1) {
        return CLIENT_TRANSPORT_CONNECT_ERROR;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        return CLIENT_TRANSPORT_CONNECT_ERROR;
    }

    /* connect() có thể chưa xong ngay; POLLOUT và SO_ERROR xác nhận kết quả. */
    if (connect(*socket_fd, (const struct sockaddr *)&address, sizeof(address)) == -1) {
        if (errno != EINPROGRESS) {
            return CLIENT_TRANSPORT_CONNECT_ERROR;
        }
        ClientTransportResult wait_result = wait_for_socket(*socket_fd, POLLOUT,
                                                             deadline_ms);
        if (wait_result != CLIENT_TRANSPORT_OK) {
            return wait_result;
        }
    }

    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    if (getsockopt(*socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == -1 ||
        socket_error != 0) {
        return CLIENT_TRANSPORT_CONNECT_ERROR;
    }
    return CLIENT_TRANSPORT_OK;
}

static ClientTransportResult send_frame(int socket_fd, const ByteBuffer *frame,
                                         int64_t deadline_ms)
{
    size_t sent_total = 0;
    while (sent_total < frame->length) {
        ClientTransportResult time_result = check_deadline(deadline_ms);
        if (time_result != CLIENT_TRANSPORT_OK) {
            return time_result;
        }
        ssize_t sent = send(socket_fd, frame->data + sent_total,
                            frame->length - sent_total, MSG_NOSIGNAL);
        if (sent > 0) {
            sent_total += (size_t)sent;
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ClientTransportResult wait_result = wait_for_socket(socket_fd, POLLOUT,
                                                                 deadline_ms);
            if (wait_result == CLIENT_TRANSPORT_OK) {
                continue;
            }
            return wait_result;
        }
        return CLIENT_TRANSPORT_IO_ERROR;
    }
    return CLIENT_TRANSPORT_OK;
}

static ClientTransportResult receive_frame(int socket_fd, int64_t deadline_ms,
                                            ByteBuffer *reply)
{
    ByteBuffer input = {0};
    if (buffer_init(&input, CHAT_INPUT_BUFFER_CAPACITY) != BUFFER_OK) {
        return CLIENT_TRANSPORT_ALLOCATION_ERROR;
    }

    ClientTransportResult result = CLIENT_TRANSPORT_OK;
    for (;;) {
        result = check_deadline(deadline_ms);
        if (result != CLIENT_TRANSPORT_OK) {
            break;
        }
        FrameView view = {0};
        FrameResult frame_result = frame_peek(&input, &view);
        if (frame_result == FRAME_OK) {
            char ping_nonce[CHAT_HEARTBEAT_NONCE_HEX_LEN + 1];
            if (protocol_parse_heartbeat_ping(view.body, view.length, ping_nonce) == PROTOCOL_OK) {
                /* Tự động phản hồi heartbeat.pong rồi tiếp tục chờ response nghiệp vụ. */
                ByteBuffer pong_frame = {0};
                if (buffer_init(&pong_frame, 256) != BUFFER_OK ||
                    protocol_encode_heartbeat_pong(ping_nonce, &pong_frame) != PROTOCOL_OK) {
                    buffer_destroy(&pong_frame);
                    result = CLIENT_TRANSPORT_ALLOCATION_ERROR;
                    break;
                }
                result = send_frame(socket_fd, &pong_frame, deadline_ms);
                buffer_destroy(&pong_frame);
                if (result != CLIENT_TRANSPORT_OK) {
                    break;
                }
                if (frame_consume(&input) != FRAME_OK) {
                    result = CLIENT_TRANSPORT_PROTOCOL_ERROR;
                    break;
                }
                continue;
            }
            if (buffer_init(reply, view.length) != BUFFER_OK ||
                buffer_append(reply, view.body, view.length) != BUFFER_OK) {
                result = CLIENT_TRANSPORT_ALLOCATION_ERROR;
            }
            break;
        }
        if (frame_result != FRAME_NEED_MORE || input.length == input.capacity) {
            result = CLIENT_TRANSPORT_PROTOCOL_ERROR;
            break;
        }

        unsigned char chunk[4096];
        size_t available = input.capacity - input.length;
        size_t read_size = available < sizeof(chunk) ? available : sizeof(chunk);
        ssize_t received = recv(socket_fd, chunk, read_size, 0);
        if (received > 0) {
            if (buffer_append(&input, chunk, (size_t)received) != BUFFER_OK) {
                result = CLIENT_TRANSPORT_PROTOCOL_ERROR;
                break;
            }
            continue;
        }
        if (received == -1 && errno == EINTR) {
            continue;
        }
        if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            result = wait_for_socket(socket_fd, POLLIN, deadline_ms);
            if (result == CLIENT_TRANSPORT_OK) {
                continue;
            }
            break;
        }
        result = CLIENT_TRANSPORT_IO_ERROR;
        break;
    }

    buffer_destroy(&input);
    if (result != CLIENT_TRANSPORT_OK) {
        buffer_destroy(reply);
    }
    return result;
}

ClientTransportResult client_roundtrip_to(const char *host, uint16_t port,
                                           const char *body, size_t length,
                                           int timeout_ms, ByteBuffer *reply)
{
    if (host == NULL || port == 0 || timeout_ms <= 0 || body == NULL ||
        reply == NULL || length == 0 ||
        length > CHAT_MAX_FRAME_BODY_BYTES || reply->data != NULL ||
        reply->length != 0 || reply->capacity != 0) {
        return CLIENT_TRANSPORT_INVALID_BODY;
    }

    int64_t start_ms;
    if (!monotonic_milliseconds(&start_ms)) {
        return CLIENT_TRANSPORT_IO_ERROR;
    }
    int64_t deadline_ms = start_ms + timeout_ms;

    ByteBuffer frame = {0};
    if (buffer_init(&frame, CHAT_FRAME_HEADER_BYTES + length) != BUFFER_OK) {
        return CLIENT_TRANSPORT_ALLOCATION_ERROR;
    }
    if (frame_encode(&frame, body, length) != FRAME_OK) {
        buffer_destroy(&frame);
        return CLIENT_TRANSPORT_INVALID_BODY;
    }

    int socket_fd = -1;
    ClientTransportResult result = connect_to_server(host, port, deadline_ms, &socket_fd);
    if (result == CLIENT_TRANSPORT_OK) {
        ByteBuffer ready = {0};
        /* Client phải kiểm tra ready/version trước khi gửi byte request đầu tiên. */
        result = receive_frame(socket_fd, deadline_ms, &ready);
        if (result == CLIENT_TRANSPORT_OK &&
            protocol_validate_ready_event(ready.data, ready.length) != PROTOCOL_OK) {
            result = CLIENT_TRANSPORT_PROTOCOL_ERROR;
        }
        buffer_destroy(&ready);
    }
    if (result == CLIENT_TRANSPORT_OK) {
        result = send_frame(socket_fd, &frame, deadline_ms);
    }
    if (result == CLIENT_TRANSPORT_OK) {
        result = receive_frame(socket_fd, deadline_ms, reply);
    }

    if (socket_fd != -1) {
        close(socket_fd);
    }
    buffer_destroy(&frame);
    return result;
}

ClientTransportResult client_roundtrip(const char *body, size_t length, ByteBuffer *reply)
{
    return client_roundtrip_to(CHAT_DEFAULT_HOST, CHAT_DEFAULT_PORT, body, length,
                               CHAT_CLIENT_ROUNDTRIP_TIMEOUT_MS, reply);
}

static bool extract_line(ByteBuffer *buf, char *line_out, size_t max_out)
{
    if (buf == NULL || buf->length == 0 || line_out == NULL || max_out == 0) {
        return false;
    }
    size_t newline_pos = 0;
    bool found = false;
    for (size_t i = 0; i < buf->length; ++i) {
        if (buf->data[i] == '\n') {
            newline_pos = i;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    size_t copy_len = newline_pos;
    if (copy_len > 0 && buf->data[copy_len - 1] == '\r') {
        --copy_len;
    }
    if (copy_len >= max_out) {
        copy_len = max_out - 1;
    }
    memcpy(line_out, buf->data, copy_len);
    line_out[copy_len] = '\0';
    buffer_consume(buf, newline_pos + 1);
    return true;
}

static ClientTransportResult send_json_request(int socket_fd, const char *json_body, int64_t deadline_ms)
{
    size_t len = strlen(json_body);
    ByteBuffer frame = {0};
    if (buffer_init(&frame, CHAT_FRAME_HEADER_BYTES + len) != BUFFER_OK) {
        return CLIENT_TRANSPORT_ALLOCATION_ERROR;
    }
    if (frame_encode(&frame, json_body, len) != FRAME_OK) {
        buffer_destroy(&frame);
        return CLIENT_TRANSPORT_INVALID_BODY;
    }
    ClientTransportResult res = send_frame(socket_fd, &frame, deadline_ms);
    buffer_destroy(&frame);
    return res;
}

static bool generate_client_msg_id(char *out_hex, size_t out_size)
{
    if (out_hex == NULL || out_size < 33) {
        return false;
    }
    unsigned char random_bytes[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
        size_t n = fread(random_bytes, 1, sizeof(random_bytes), f);
        fclose(f);
        if (n == sizeof(random_bytes)) {
            for (int i = 0; i < 16; ++i) {
                snprintf(out_hex + (i * 2), 3, "%02x", random_bytes[i]);
            }
            out_hex[32] = '\0';
            return true;
        }
    }
    for (int i = 0; i < 32; ++i) {
        out_hex[i] = "0123456789abcdef"[rand() % 16];
    }
    out_hex[32] = '\0';
    return true;
}

ClientTransportResult client_interactive_loop(const char *host, uint16_t port,
                                              int in_fd, int out_fd)
{
    if (host == NULL || port == 0 || in_fd < 0 || out_fd < 0) {
        return CLIENT_TRANSPORT_INVALID_BODY;
    }

    int64_t start_ms;
    if (!monotonic_milliseconds(&start_ms)) {
        return CLIENT_TRANSPORT_IO_ERROR;
    }
    int64_t deadline_ms = start_ms + CHAT_CLIENT_ROUNDTRIP_TIMEOUT_MS;

    int socket_fd = -1;
    ClientTransportResult result = connect_to_server(host, port, deadline_ms, &socket_fd);
    if (result != CLIENT_TRANSPORT_OK) {
        return result;
    }

    ByteBuffer ready = {0};
    result = receive_frame(socket_fd, deadline_ms, &ready);
    if (result != CLIENT_TRANSPORT_OK) {
        close(socket_fd);
        return result;
    }
    if (protocol_validate_ready_event(ready.data, ready.length) != PROTOCOL_OK) {
        buffer_destroy(&ready);
        close(socket_fd);
        return CLIENT_TRANSPORT_PROTOCOL_ERROR;
    }
    buffer_destroy(&ready);

    dprintf(out_fd, "Connected to %s:%u (protocol v1 ready).\n", host, (unsigned)port);
    dprintf(out_fd, "Commands: /login, /register, /send, /chat, /back, /history, /ping, /close, /quit or raw JSON.\n");

    ByteBuffer socket_input = {0};
    ByteBuffer stdin_input = {0};
    if (buffer_init(&socket_input, CHAT_INPUT_BUFFER_CAPACITY) != BUFFER_OK ||
        buffer_init(&stdin_input, 1024) != BUFFER_OK) {
        buffer_destroy(&socket_input);
        buffer_destroy(&stdin_input);
        close(socket_fd);
        return CLIENT_TRANSPORT_ALLOCATION_ERROR;
    }

    uint64_t next_request_id = 1;
    bool running = true;
    bool close_requested = false;
    int64_t close_deadline_ms = 0;

    int64_t dedup_cache[1024];
    size_t dedup_count = 0;
    size_t dedup_head = 0;

    int64_t group_dedup_cache[1024];
    size_t group_dedup_count = 0;
    size_t group_dedup_head = 0;

    char active_chat_peer_id[32] = {0};
    char active_group_id[32] = {0};

    while (running) {
        int64_t now_ms;
        if (!monotonic_milliseconds(&now_ms)) {
            result = CLIENT_TRANSPORT_IO_ERROR;
            break;
        }

        if (close_requested && now_ms >= close_deadline_ms) {
            break;
        }

        struct pollfd fds[2];
        fds[0].fd = in_fd;
        fds[0].events = (!close_requested) ? POLLIN : 0;
        fds[0].revents = 0;

        fds[1].fd = socket_fd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int timeout = close_requested ? 100 : 500;
        int poll_rc = poll(fds, 2, timeout);
        if (poll_rc == -1) {
            if (errno == EINTR) {
                continue;
            }
            result = CLIENT_TRANSPORT_IO_ERROR;
            break;
        }

        /* 1. Xử lý dữ liệu đến từ socket */
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            unsigned char chunk[4096];
            size_t available = socket_input.capacity - socket_input.length;
            size_t to_read = available < sizeof(chunk) ? available : sizeof(chunk);
            ssize_t n = recv(socket_fd, chunk, to_read, 0);
            if (n > 0) {
                if (buffer_append(&socket_input, chunk, (size_t)n) != BUFFER_OK) {
                    result = CLIENT_TRANSPORT_PROTOCOL_ERROR;
                    break;
                }

                FrameView view = {0};
                while (frame_peek(&socket_input, &view) == FRAME_OK) {
                    char ping_nonce[CHAT_HEARTBEAT_NONCE_HEX_LEN + 1];
                    if (protocol_parse_heartbeat_ping(view.body, view.length, ping_nonce) == PROTOCOL_OK) {
                        ByteBuffer pong_frame = {0};
                        if (buffer_init(&pong_frame, 256) == BUFFER_OK &&
                            protocol_encode_heartbeat_pong(ping_nonce, &pong_frame) == PROTOCOL_OK) {
                            int64_t pong_now;
                            monotonic_milliseconds(&pong_now);
                            send_frame(socket_fd, &pong_frame, pong_now + 5000);
                        }
                        buffer_destroy(&pong_frame);
                        dprintf(out_fd, "[Heartbeat] Responded pong (%s)\n", ping_nonce);
                    } else {
                        dprintf(out_fd, "< %.*s\n", (int)view.length, (const char *)view.body);
                        cJSON *doc = cJSON_ParseWithLength((const char *)view.body, view.length);
                        if (doc != NULL) {
                            const cJSON *kind = cJSON_GetObjectItemCaseSensitive(doc, "kind");
                            const cJSON *type = cJSON_GetObjectItemCaseSensitive(doc, "type");
                            const cJSON *ok = cJSON_GetObjectItemCaseSensitive(doc, "ok");
                            const cJSON *data = cJSON_GetObjectItemCaseSensitive(doc, "data");

                            if (kind != NULL && strcmp(kind->valuestring, "response") == 0 &&
                                type != NULL && strcmp(type->valuestring, "connection.close") == 0 &&
                                cJSON_IsTrue(ok)) {
                                running = false;
                            } else if (kind != NULL && strcmp(kind->valuestring, "event") == 0 &&
                                       type != NULL && strcmp(type->valuestring, "direct.message") == 0 &&
                                       data != NULL && cJSON_IsObject(data)) {
                                const cJSON *mid_item = cJSON_GetObjectItemCaseSensitive(data, "message_id");
                                const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(data, "text");
                                const cJSON *sender_obj = cJSON_GetObjectItemCaseSensitive(data, "sender");
                                const cJSON *sname_item = sender_obj != NULL ? cJSON_GetObjectItemCaseSensitive(sender_obj, "username") : NULL;
                                const cJSON *sid_item = sender_obj != NULL ? cJSON_GetObjectItemCaseSensitive(sender_obj, "user_id") : NULL;

                                if (mid_item != NULL && cJSON_IsString(mid_item) && mid_item->valuestring != NULL) {
                                    const char *mid_str = mid_item->valuestring;
                                    char *endptr = NULL;
                                    long long mid_val = strtoll(mid_str, &endptr, 10);

                                    /* Kiểm tra dedup */
                                    bool already_received = false;
                                    for (size_t di = 0; di < dedup_count; ++di) {
                                        if (dedup_cache[di] == (int64_t)mid_val) {
                                            already_received = true;
                                            break;
                                        }
                                    }

                                    if (!already_received) {
                                        if (dedup_count < 1024) {
                                            dedup_cache[dedup_count++] = (int64_t)mid_val;
                                        } else {
                                            dedup_cache[dedup_head] = (int64_t)mid_val;
                                            dedup_head = (dedup_head + 1) % 1024;
                                        }

                                        const char *sender_name = (sname_item && sname_item->valuestring) ? sname_item->valuestring : "Unknown";
                                        const char *sender_id_str = (sid_item && sid_item->valuestring) ? sid_item->valuestring : "?";
                                        const char *msg_text = (text_item && text_item->valuestring) ? text_item->valuestring : "";

                                        dprintf(out_fd, "[Message from %s (%s)]: %s\n", sender_name, sender_id_str, msg_text);
                                    }

                                    /* Tự động gửi delivery.ack theo spec Q03 và mục 7 */
                                    char ack_body[256];
                                    snprintf(ack_body, sizeof(ack_body),
                                             "{\"v\":1,\"kind\":\"request\",\"type\":\"delivery.ack\","
                                             "\"request_id\":\"%" PRIu64 "\",\"data\":{\"messages\":[{\"scope\":\"direct\",\"message_id\":\"%s\"}]}}",
                                             next_request_id++, mid_str);
                                    dprintf(out_fd, "> %s\n", ack_body);
                                    int64_t ack_now;
                                    monotonic_milliseconds(&ack_now);
                                    send_json_request(socket_fd, ack_body, ack_now + 5000);
                                    dprintf(out_fd, "[Auto-ACK] Sent delivery.ack for message_id %s\n", mid_str);
                                }
                            } else if (kind != NULL && strcmp(kind->valuestring, "event") == 0 &&
                                       type != NULL && strcmp(type->valuestring, "group.message") == 0 &&
                                       data != NULL && cJSON_IsObject(data)) {
                                const cJSON *mid_item = cJSON_GetObjectItemCaseSensitive(data, "message_id");
                                const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(data, "text");
                                const cJSON *gid_item = cJSON_GetObjectItemCaseSensitive(data, "group_id");
                                const cJSON *gname_item = cJSON_GetObjectItemCaseSensitive(data, "group_name");
                                const cJSON *sender_obj = cJSON_GetObjectItemCaseSensitive(data, "sender");
                                const cJSON *sname_item = sender_obj != NULL ? cJSON_GetObjectItemCaseSensitive(sender_obj, "username") : NULL;
                                const cJSON *sid_item = sender_obj != NULL ? cJSON_GetObjectItemCaseSensitive(sender_obj, "user_id") : NULL;

                                if (mid_item != NULL && cJSON_IsString(mid_item) && mid_item->valuestring != NULL) {
                                    const char *mid_str = mid_item->valuestring;
                                    char *endptr = NULL;
                                    long long mid_val = strtoll(mid_str, &endptr, 10);

                                    /* Kiểm tra dedup cho group */
                                    bool already_received = false;
                                    for (size_t di = 0; di < group_dedup_count; ++di) {
                                        if (group_dedup_cache[di] == (int64_t)mid_val) {
                                            already_received = true;
                                            break;
                                        }
                                    }

                                    if (!already_received) {
                                        if (group_dedup_count < 1024) {
                                            group_dedup_cache[group_dedup_count++] = (int64_t)mid_val;
                                        } else {
                                            group_dedup_cache[group_dedup_head] = (int64_t)mid_val;
                                            group_dedup_head = (group_dedup_head + 1) % 1024;
                                        }

                                        const char *group_name = (gname_item && gname_item->valuestring) ? gname_item->valuestring : "Group";
                                        const char *group_id_str = (gid_item && gid_item->valuestring) ? gid_item->valuestring : "?";
                                        const char *sender_name = (sname_item && sname_item->valuestring) ? sname_item->valuestring : "Unknown";
                                        const char *sender_id_str = (sid_item && sid_item->valuestring) ? sid_item->valuestring : "?";
                                        const char *msg_text = (text_item && text_item->valuestring) ? text_item->valuestring : "";

                                        dprintf(out_fd, "[Group %s (%s) from %s (%s)]: %s\n", group_name, group_id_str, sender_name, sender_id_str, msg_text);
                                    }

                                    /* Tự động gửi delivery.ack cho group theo spec Q03 và mục 7 */
                                    char ack_body[256];
                                    snprintf(ack_body, sizeof(ack_body),
                                             "{\"v\":1,\"kind\":\"request\",\"type\":\"delivery.ack\","
                                             "\"request_id\":\"%" PRIu64 "\",\"data\":{\"messages\":[{\"scope\":\"group\",\"message_id\":\"%s\"}]}}",
                                             next_request_id++, mid_str);
                                    dprintf(out_fd, "> %s\n", ack_body);
                                    int64_t ack_now;
                                    monotonic_milliseconds(&ack_now);
                                    send_json_request(socket_fd, ack_body, ack_now + 5000);
                                    dprintf(out_fd, "[Auto-ACK] Sent delivery.ack for group message_id %s\n", mid_str);
                                }
                            } else if (kind != NULL && strcmp(kind->valuestring, "event") == 0 &&
                                       type != NULL && strcmp(type->valuestring, "group.invite.received") == 0 &&
                                       data != NULL && cJSON_IsObject(data)) {
                                const cJSON *invid = cJSON_GetObjectItemCaseSensitive(data, "invitation_id");
                                const cJSON *gid = cJSON_GetObjectItemCaseSensitive(data, "group_id");
                                const cJSON *gname = cJSON_GetObjectItemCaseSensitive(data, "group_name");
                                const cJSON *inviter = cJSON_GetObjectItemCaseSensitive(data, "inviter");
                                const cJSON *iname = inviter ? cJSON_GetObjectItemCaseSensitive(inviter, "username") : NULL;
                                dprintf(out_fd, "[Group Invite Received] invitation_id=%s, group=%s (%s), inviter=%s\n",
                                        invid && invid->valuestring ? invid->valuestring : "?",
                                        gname && gname->valuestring ? gname->valuestring : "?",
                                        gid && gid->valuestring ? gid->valuestring : "?",
                                        iname && iname->valuestring ? iname->valuestring : "?");
                            } else if (kind != NULL && strcmp(kind->valuestring, "event") == 0 &&
                                       type != NULL && strcmp(type->valuestring, "group.invite.resolved") == 0 &&
                                       data != NULL && cJSON_IsObject(data)) {
                                const cJSON *invid = cJSON_GetObjectItemCaseSensitive(data, "invitation_id");
                                const cJSON *st = cJSON_GetObjectItemCaseSensitive(data, "status");
                                dprintf(out_fd, "[Group Invite Resolved] invitation_id=%s, status=%s\n",
                                        invid && invid->valuestring ? invid->valuestring : "?",
                                        st && st->valuestring ? st->valuestring : "?");
                            } else if (kind != NULL && strcmp(kind->valuestring, "event") == 0 &&
                                       type != NULL && strcmp(type->valuestring, "group.member.changed") == 0 &&
                                       data != NULL && cJSON_IsObject(data)) {
                                const cJSON *gid = cJSON_GetObjectItemCaseSensitive(data, "group_id");
                                const cJSON *ch = cJSON_GetObjectItemCaseSensitive(data, "change");
                                const cJSON *uid = cJSON_GetObjectItemCaseSensitive(data, "user_id");
                                dprintf(out_fd, "[Group Member Changed] group_id=%s, user_id=%s, change=%s\n",
                                        gid && gid->valuestring ? gid->valuestring : "?",
                                        uid && uid->valuestring ? uid->valuestring : "?",
                                        ch && ch->valuestring ? ch->valuestring : "?");
                            } else if (kind != NULL && strcmp(kind->valuestring, "event") == 0 &&
                                       type != NULL && strcmp(type->valuestring, "group.owner.changed") == 0 &&
                                       data != NULL && cJSON_IsObject(data)) {
                                const cJSON *gid = cJSON_GetObjectItemCaseSensitive(data, "group_id");
                                const cJSON *noid = cJSON_GetObjectItemCaseSensitive(data, "new_owner_user_id");
                                dprintf(out_fd, "[Group Owner Changed] group_id=%s, new_owner_id=%s\n",
                                        gid && gid->valuestring ? gid->valuestring : "?",
                                        noid && noid->valuestring ? noid->valuestring : "?");
                            } else if (kind != NULL && strcmp(kind->valuestring, "event") == 0 &&
                                       type != NULL && strcmp(type->valuestring, "group.closed") == 0 &&
                                       data != NULL && cJSON_IsObject(data)) {
                                const cJSON *gid = cJSON_GetObjectItemCaseSensitive(data, "group_id");
                                const cJSON *gname = cJSON_GetObjectItemCaseSensitive(data, "group_name");
                                const cJSON *buid = cJSON_GetObjectItemCaseSensitive(data, "by_user_id");
                                dprintf(out_fd, "[Group Closed] Group %s (%s) closed by owner %s\n",
                                        gname && gname->valuestring ? gname->valuestring : "?",
                                        gid && gid->valuestring ? gid->valuestring : "?",
                                        buid && buid->valuestring ? buid->valuestring : "?");
                            }
                            cJSON_Delete(doc);
                        }
                    }
                    frame_consume(&socket_input);
                }
            } else if (n == 0) {
                dprintf(out_fd, "Server closed connection.\n");
                running = false;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                result = CLIENT_TRANSPORT_IO_ERROR;
                break;
            }
        }

        /* 2. Xử lý dữ liệu nhập từ in_fd (stdin hoặc pipe) */
        if (!close_requested && (fds[0].revents & (POLLIN | POLLHUP))) {
            char in_chunk[256];
            ssize_t in_n = read(in_fd, in_chunk, sizeof(in_chunk));
            if (in_n > 0) {
                if (buffer_append(&stdin_input, (const unsigned char *)in_chunk, (size_t)in_n) == BUFFER_OK) {
                    char line[512];
                    while (extract_line(&stdin_input, line, sizeof(line))) {
                        /* Bỏ khoảng trắng đầu */
                        char *start = line;
                        while (*start == ' ' || *start == '\t') {
                            ++start;
                        }
                        if (*start == '\0') {
                            continue;
                        }

                        if (strcmp(start, "/ping") == 0) {
                            char body[256];
                            snprintf(body, sizeof(body),
                                     "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                                     "\"request_id\":\"%" PRIu64 "\",\"data\":{}}",
                                     next_request_id++);
                            dprintf(out_fd, "> %s\n", body);
                            int64_t send_now;
                            monotonic_milliseconds(&send_now);
                            send_json_request(socket_fd, body, send_now + 5000);
                        } else if (strcmp(start, "/close") == 0 || strcmp(start, "/quit") == 0) {
                            char body[256];
                            snprintf(body, sizeof(body),
                                     "{\"v\":1,\"kind\":\"request\",\"type\":\"connection.close\","
                                     "\"request_id\":\"%" PRIu64 "\",\"data\":{}}",
                                     next_request_id++);
                            dprintf(out_fd, "> %s\n", body);
                            int64_t send_now;
                            monotonic_milliseconds(&send_now);
                            send_json_request(socket_fd, body, send_now + 5000);
                            close_requested = true;
                            close_deadline_ms = send_now + 2000;
                        } else if (strncmp(start, "/login ", 7) == 0) {
                            char username[64] = {0};
                            char password[64] = {0};
                            if (sscanf(start + 7, "%63s %63s", username, password) == 2) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"auth.login\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"username\":\"%s\",\"password\":\"%s\"}}",
                                         next_request_id++, username, password);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /login <username> <password>\n");
                            }
                        } else if (strncmp(start, "/register ", 10) == 0) {
                            char username[64] = {0};
                            char password[64] = {0};
                            if (sscanf(start + 10, "%63s %63s", username, password) == 2) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"auth.register\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"username\":\"%s\",\"password\":\"%s\"}}",
                                         next_request_id++, username, password);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /register <username> <password>\n");
                            }
                        } else if (strncmp(start, "/send ", 6) == 0) {
                            char recip_id[32] = {0};
                            char *msg_text = NULL;
                            char *space_pos = strchr(start + 6, ' ');
                            if (space_pos != NULL) {
                                size_t id_len = space_pos - (start + 6);
                                if (id_len < sizeof(recip_id)) {
                                    memcpy(recip_id, start + 6, id_len);
                                    recip_id[id_len] = '\0';
                                    msg_text = space_pos + 1;
                                    while (*msg_text == ' ') {
                                        ++msg_text;
                                    }
                                }
                            }
                            if (recip_id[0] != '\0' && msg_text != NULL && *msg_text != '\0') {
                                char cid[CHAT_CLIENT_MSG_ID_HEX_LEN + 1];
                                generate_client_msg_id(cid, sizeof(cid));
                                char body[1024];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"direct.send\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"recipient_user_id\":\"%s\","
                                         "\"client_msg_id\":\"%s\",\"text\":\"%s\"}}",
                                         next_request_id++, recip_id, cid, msg_text);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /send <recipient_user_id> <text>\n");
                            }
                        } else if (strncmp(start, "/chat ", 6) == 0) {
                            char *peer = start + 6;
                            while (*peer == ' ') {
                                ++peer;
                            }
                            if (*peer != '\0') {
                                snprintf(active_chat_peer_id, sizeof(active_chat_peer_id), "%s", peer);
                                dprintf(out_fd, "Entered chat mode with user %s. Type messages to send, or /back to exit.\n",
                                        active_chat_peer_id);
                            } else {
                                dprintf(out_fd, "Usage: /chat <peer_user_id>\n");
                            }
                        } else if (strcmp(start, "/back") == 0) {
                            if (active_chat_peer_id[0] != '\0') {
                                dprintf(out_fd, "Exited chat mode with user %s.\n", active_chat_peer_id);
                                active_chat_peer_id[0] = '\0';
                            } else if (active_group_id[0] != '\0') {
                                dprintf(out_fd, "Exited group chat mode for group %s.\n", active_group_id);
                                active_group_id[0] = '\0';
                            } else {
                                dprintf(out_fd, "Not currently in chat mode.\n");
                            }
                        } else if (strncmp(start, "/group_create ", 14) == 0) {
                            char gname[64] = {0};
                            char vis[32] = {0};
                            if (sscanf(start + 14, "%63s %31s", gname, vis) == 2) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.create\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"name\":\"%s\",\"visibility\":\"%s\"}}",
                                         next_request_id++, gname, vis);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_create <name> <public|private>\n");
                            }
                        } else if (strncmp(start, "/group_list", 11) == 0) {
                            char after_id[32] = {0};
                            int parts = sscanf(start + 11, "%31s", after_id);
                            char body[256];
                            if (parts >= 1 && after_id[0] != '\0') {
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.list\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"after_id\":\"%s\"}}",
                                         next_request_id++, after_id);
                            } else {
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.list\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{}}",
                                         next_request_id++);
                            }
                            dprintf(out_fd, "> %s\n", body);
                            int64_t send_now;
                            monotonic_milliseconds(&send_now);
                            send_json_request(socket_fd, body, send_now + 5000);
                        } else if (strncmp(start, "/group_closed_list", 18) == 0) {
                            char after_id[32] = {0};
                            int parts = sscanf(start + 18, "%31s", after_id);
                            char body[256];
                            if (parts >= 1 && after_id[0] != '\0') {
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.closed.list\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"after_id\":\"%s\"}}",
                                         next_request_id++, after_id);
                            } else {
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.closed.list\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{}}",
                                         next_request_id++);
                            }
                            dprintf(out_fd, "> %s\n", body);
                            int64_t send_now;
                            monotonic_milliseconds(&send_now);
                            send_json_request(socket_fd, body, send_now + 5000);
                        } else if (strncmp(start, "/group_members ", 15) == 0) {
                            char gid[32] = {0};
                            char after_id[32] = {0};
                            int parts = sscanf(start + 15, "%31s %31s", gid, after_id);
                            if (parts >= 1) {
                                char body[256];
                                if (parts >= 2 && after_id[0] != '\0') {
                                    snprintf(body, sizeof(body),
                                             "{\"v\":1,\"kind\":\"request\",\"type\":\"group.members\","
                                             "\"request_id\":\"%" PRIu64 "\",\"data\":{\"group_id\":\"%s\",\"after_id\":\"%s\"}}",
                                             next_request_id++, gid, after_id);
                                } else {
                                    snprintf(body, sizeof(body),
                                             "{\"v\":1,\"kind\":\"request\",\"type\":\"group.members\","
                                             "\"request_id\":\"%" PRIu64 "\",\"data\":{\"group_id\":\"%s\"}}",
                                             next_request_id++, gid);
                                }
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_members <group_id> [after_id]\n");
                            }
                        } else if (strncmp(start, "/group_invite ", 14) == 0) {
                            char gid[32] = {0};
                            char target_username[64] = {0};
                            if (sscanf(start + 14, "%31s %63s", gid, target_username) == 2) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.invite.send\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"group_id\":\"%s\",\"username\":\"%s\"}}",
                                         next_request_id++, gid, target_username);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_invite <group_id> <username>\n");
                            }
                        } else if (strncmp(start, "/group_invites", 14) == 0) {
                            char after_id[32] = {0};
                            int parts = sscanf(start + 14, "%31s", after_id);
                            char body[256];
                            if (parts >= 1 && after_id[0] != '\0') {
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.invite.list\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"after_id\":\"%s\"}}",
                                         next_request_id++, after_id);
                            } else {
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.invite.list\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{}}",
                                         next_request_id++);
                            }
                            dprintf(out_fd, "> %s\n", body);
                            int64_t send_now;
                            monotonic_milliseconds(&send_now);
                            send_json_request(socket_fd, body, send_now + 5000);
                        } else if (strncmp(start, "/group_accept ", 14) == 0) {
                            char inv_id[32] = {0};
                            if (sscanf(start + 14, "%31s", inv_id) == 1) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.invite.accept\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"invitation_id\":\"%s\"}}",
                                         next_request_id++, inv_id);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_accept <invitation_id>\n");
                            }
                        } else if (strncmp(start, "/group_reject ", 14) == 0) {
                            char inv_id[32] = {0};
                            if (sscanf(start + 14, "%31s", inv_id) == 1) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.invite.reject\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"invitation_id\":\"%s\"}}",
                                         next_request_id++, inv_id);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_reject <invitation_id>\n");
                            }
                        } else if (strncmp(start, "/group_remove ", 14) == 0) {
                            char gid[32] = {0};
                            char mem_uid[32] = {0};
                            if (sscanf(start + 14, "%31s %31s", gid, mem_uid) == 2) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.member.remove\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"group_id\":\"%s\",\"member_user_id\":\"%s\"}}",
                                         next_request_id++, gid, mem_uid);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_remove <group_id> <member_user_id>\n");
                            }
                        } else if (strncmp(start, "/group_transfer ", 16) == 0) {
                            char gid[32] = {0};
                            char new_owner[32] = {0};
                            if (sscanf(start + 16, "%31s %31s", gid, new_owner) == 2) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.owner.transfer\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"group_id\":\"%s\",\"new_owner_user_id\":\"%s\"}}",
                                         next_request_id++, gid, new_owner);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_transfer <group_id> <new_owner_user_id>\n");
                            }
                        } else if (strncmp(start, "/group_leave ", 13) == 0) {
                            char gid[32] = {0};
                            if (sscanf(start + 13, "%31s", gid) == 1) {
                                char body[256];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.leave\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"group_id\":\"%s\"}}",
                                         next_request_id++, gid);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_leave <group_id>\n");
                            }
                        } else if (strncmp(start, "/group_send ", 12) == 0) {
                            char gid[32] = {0};
                            char *msg_text = NULL;
                            char *space_pos = strchr(start + 12, ' ');
                            if (space_pos != NULL) {
                                size_t id_len = space_pos - (start + 12);
                                if (id_len < sizeof(gid)) {
                                    memcpy(gid, start + 12, id_len);
                                    gid[id_len] = '\0';
                                    msg_text = space_pos + 1;
                                    while (*msg_text == ' ') {
                                        ++msg_text;
                                    }
                                }
                            }
                            if (gid[0] != '\0' && msg_text != NULL && *msg_text != '\0') {
                                char cid[CHAT_CLIENT_MSG_ID_HEX_LEN + 1];
                                generate_client_msg_id(cid, sizeof(cid));
                                char body[1024];
                                snprintf(body, sizeof(body),
                                         "{\"v\":1,\"kind\":\"request\",\"type\":\"group.send\","
                                         "\"request_id\":\"%" PRIu64 "\",\"data\":{\"group_id\":\"%s\","
                                         "\"client_msg_id\":\"%s\",\"text\":\"%s\"}}",
                                         next_request_id++, gid, cid, msg_text);
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /group_send <group_id> <text>\n");
                            }
                        } else if (strncmp(start, "/group_chat ", 12) == 0) {
                            char *gid = start + 12;
                            while (*gid == ' ') {
                                ++gid;
                            }
                            if (*gid != '\0') {
                                snprintf(active_group_id, sizeof(active_group_id), "%s", gid);
                                active_chat_peer_id[0] = '\0';
                                dprintf(out_fd, "Entered group chat mode for group %s. Type messages to send, or /back to exit.\n",
                                        active_group_id);
                            } else {
                                dprintf(out_fd, "Usage: /group_chat <group_id>\n");
                            }
                        } else if (strncmp(start, "/history ", 9) == 0) {
                            char peer_id[32] = {0};
                            char after_id[32] = {0};
                            int parts = sscanf(start + 9, "%31s %31s", peer_id, after_id);
                            if (parts >= 1) {
                                char body[512];
                                if (parts == 1) {
                                    snprintf(body, sizeof(body),
                                             "{\"v\":1,\"kind\":\"request\",\"type\":\"direct.history\","
                                             "\"request_id\":\"%" PRIu64 "\",\"data\":{\"peer_user_id\":\"%s\"}}",
                                             next_request_id++, peer_id);
                                } else {
                                    snprintf(body, sizeof(body),
                                             "{\"v\":1,\"kind\":\"request\",\"type\":\"direct.history\","
                                             "\"request_id\":\"%" PRIu64 "\",\"data\":{\"peer_user_id\":\"%s\",\"after_id\":\"%s\"}}",
                                             next_request_id++, peer_id, after_id);
                                }
                                dprintf(out_fd, "> %s\n", body);
                                int64_t send_now;
                                monotonic_milliseconds(&send_now);
                                send_json_request(socket_fd, body, send_now + 5000);
                            } else {
                                dprintf(out_fd, "Usage: /history <peer_user_id> [after_id]\n");
                            }
                        } else if (strcmp(start, "/help") == 0) {
                            dprintf(out_fd, "Available commands:\n"
                                            "  /login <user> <pass>        Login\n"
                                            "  /register <user> <pass>     Register account\n"
                                            "  /send <peer_id> <text>      Send direct message\n"
                                            "  /chat <peer_id>             Enter 1-on-1 chat mode\n"
                                            "  /history <peer_id> [after]  Query message history\n"
                                            "  /group_create <name> <pub|priv>  Create group\n"
                                            "  /group_list [after]         List open groups\n"
                                            "  /group_closed_list [after]  List closed groups\n"
                                            "  /group_members <gid> [after] List group members\n"
                                            "  /group_invite <gid> <user>  Invite to group\n"
                                            "  /group_invites [after]      List pending invites\n"
                                            "  /group_accept <inv_id>      Accept group invite\n"
                                            "  /group_reject <inv_id>      Reject group invite\n"
                                            "  /group_send <gid> <text>    Send message to group\n"
                                            "  /group_chat <gid>           Enter group chat mode\n"
                                            "  /group_leave <gid>          Leave group\n"
                                            "  /group_remove <gid> <uid>   Remove member (owner only)\n"
                                            "  /group_transfer <gid> <uid> Transfer ownership\n"
                                            "  /back                       Exit chat mode\n"
                                            "  /ping                       Ping server\n"
                                            "  /close or /quit             Close session and exit\n"
                                            "  <JSON>                      Raw JSON request\n");
                        } else if (start[0] == '{') {
                            dprintf(out_fd, "> %s\n", start);
                            int64_t send_now;
                            monotonic_milliseconds(&send_now);
                            send_json_request(socket_fd, start, send_now + 5000);
                        } else if (active_chat_peer_id[0] != '\0') {
                            /* Đang trong chế độ chat 1-1: tự động gửi tin nhắn */
                            char cid[CHAT_CLIENT_MSG_ID_HEX_LEN + 1];
                            generate_client_msg_id(cid, sizeof(cid));
                            char body[1024];
                            snprintf(body, sizeof(body),
                                     "{\"v\":1,\"kind\":\"request\",\"type\":\"direct.send\","
                                     "\"request_id\":\"%" PRIu64 "\",\"data\":{\"recipient_user_id\":\"%s\","
                                     "\"client_msg_id\":\"%s\",\"text\":\"%s\"}}",
                                     next_request_id++, active_chat_peer_id, cid, start);
                            dprintf(out_fd, "> %s\n", body);
                            int64_t send_now;
                            monotonic_milliseconds(&send_now);
                            send_json_request(socket_fd, body, send_now + 5000);
                        } else if (active_group_id[0] != '\0') {
                            /* Đang trong chế độ group chat: tự động gửi tin nhắn nhóm */
                            char cid[CHAT_CLIENT_MSG_ID_HEX_LEN + 1];
                            generate_client_msg_id(cid, sizeof(cid));
                            char body[1024];
                            snprintf(body, sizeof(body),
                                     "{\"v\":1,\"kind\":\"request\",\"type\":\"group.send\","
                                     "\"request_id\":\"%" PRIu64 "\",\"data\":{\"group_id\":\"%s\","
                                     "\"client_msg_id\":\"%s\",\"text\":\"%s\"}}",
                                     next_request_id++, active_group_id, cid, start);
                            dprintf(out_fd, "> %s\n", body);
                            int64_t send_now;
                            monotonic_milliseconds(&send_now);
                            send_json_request(socket_fd, body, send_now + 5000);
                        } else {
                            dprintf(out_fd, "Unknown command '%s'. Type /help for commands or enter JSON.\n",
                                    start);
                        }
                    }
                }
            } else if (in_n == 0) {
                /* Stdin EOF (ví dụ Ctrl+D) */
                char body[256];
                snprintf(body, sizeof(body),
                         "{\"v\":1,\"kind\":\"request\",\"type\":\"connection.close\","
                         "\"request_id\":\"%" PRIu64 "\",\"data\":{}}",
                         next_request_id++);
                dprintf(out_fd, "Exiting...\n> %s\n", body);
                int64_t send_now;
                monotonic_milliseconds(&send_now);
                send_json_request(socket_fd, body, send_now + 5000);
                close_requested = true;
                close_deadline_ms = send_now + 2000;
            }
        }
    }

    buffer_destroy(&socket_input);
    buffer_destroy(&stdin_input);
    close(socket_fd);
    return result;
}

const char *client_transport_result_name(ClientTransportResult result)
{
    switch (result) {
    case CLIENT_TRANSPORT_OK:
        return "success";
    case CLIENT_TRANSPORT_INVALID_BODY:
        return "invalid endpoint, timeout, body or reply buffer";
    case CLIENT_TRANSPORT_ALLOCATION_ERROR:
        return "memory allocation failed";
    case CLIENT_TRANSPORT_CONNECT_ERROR:
        return "cannot connect to server";
    case CLIENT_TRANSPORT_IO_ERROR:
        return "socket closed or I/O failed";
    case CLIENT_TRANSPORT_PROTOCOL_ERROR:
        return "invalid connection.ready or response frame";
    case CLIENT_TRANSPORT_TIMEOUT:
        return "server did not respond in time";
    }
    return "unknown error";
}
