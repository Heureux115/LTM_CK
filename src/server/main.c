/* Cần khai báo sigaction() theo POSIX. */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "server/core/delivery_scheduler.h"
#include "server/core/router.h"
#include "server/core/session.h"
#include "server/db/db.h"
#include "server/logging/logger.h"
#include "server/net/connection.h"
#include "server/net/listener.h"
#include "shared/limits.h"
#include "shared/protocol.h"

/* Signal handler chỉ đặt cờ; vòng lặp chính mới đóng socket và dọn tài nguyên. */
static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static const char *disconnect_reason(ConnectionReceiveResult result)
{
    switch (result) {
    case CONNECTION_RECEIVE_EOF:
        return "peer EOF";
    case CONNECTION_RECEIVE_INVALID_FRAME:
        return "invalid frame";
    case CONNECTION_RECEIVE_IO_ERROR:
        return "socket error";
    case CONNECTION_RECEIVE_INPUT_FULL:
        return "input buffer full";
    case CONNECTION_RECEIVE_OUTPUT_FULL:
        return "response output buffer full";
    case CONNECTION_RECEIVE_OUTPUT_TIMEOUT:
        return "response output stalled";
    case CONNECTION_RECEIVE_CLOSE_AFTER_FLUSH:
        return "final response flushed or close deadline reached";
    case CONNECTION_RECEIVE_HEARTBEAT_TIMEOUT:
        return "heartbeat pong timeout";
    case CONNECTION_RECEIVE_DELIVERY_ACK_TIMEOUT:
        return "delivery ACK timeout";
    case CONNECTION_RECEIVE_OK:
        return "none";
    }
    return "unknown";
}

typedef struct {
    Connection *connections;
    size_t *connection_count;
    SessionManager *session_mgr;
} ServerNotificationContext;

static Connection *find_online_connection(ServerNotificationContext *ctx, int64_t user_id)
{
    SessionEntry session;
    if (session_manager_find_by_user(ctx->session_mgr, user_id, &session) != SESSION_OK) {
        return NULL;
    }
    for (size_t i = 0; i < *ctx->connection_count; ++i) {
        Connection *c = &ctx->connections[i];
        char conn_id[32];
        snprintf(conn_id, sizeof(conn_id), "conn-%d", c->fd);
        if (strcmp(conn_id, session.connection_id) == 0 && c->generation == session.generation) {
            return c;
        }
    }
    return NULL;
}

static bool server_dispatch_notification(void *user_data, int64_t recipient_user_id, const ByteBuffer *frame_buf)
{
    ServerNotificationContext *ctx = (ServerNotificationContext *)user_data;
    if (ctx == NULL || ctx->connections == NULL || ctx->session_mgr == NULL ||
        ctx->connection_count == NULL || frame_buf == NULL || frame_buf->data == NULL) {
        return false;
    }
    Connection *c = find_online_connection(ctx, recipient_user_id);
    if (c == NULL || c->peer_eof || c->close_after_flush) {
        return false;
    }
    if (c->output_offset != 0) {
        if (buffer_consume(&c->output, c->output_offset) != BUFFER_OK) {
            return false;
        }
        c->output_offset = 0;
    }
    if (buffer_append(&c->output, frame_buf->data, frame_buf->length) == BUFFER_OK) {
        if (c->output_last_progress_ms == 0) {
            struct timespec ts;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
                c->output_last_progress_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
            }
        }
        return true;
    }
    return false;
}

static bool server_dispatch_delivery_push(void *user_data,
                                          int64_t recipient_user_id,
                                          const char *scope,
                                          int64_t message_id,
                                          const ByteBuffer *frame_buf)
{
    ServerNotificationContext *ctx = (ServerNotificationContext *)user_data;
    if (ctx == NULL || ctx->connections == NULL || ctx->session_mgr == NULL ||
        ctx->connection_count == NULL || scope == NULL || message_id <= 0 ||
        frame_buf == NULL || frame_buf->data == NULL) {
        return false;
    }
    Connection *c = find_online_connection(ctx, recipient_user_id);
    if (c == NULL || c->peer_eof || c->close_after_flush || c->delivery_fatal_timeout) {
        return false;
    }
    if (delivery_tracker_has_message(&c->delivery_tracker, message_id, scope) ||
        delivery_tracker_is_full(&c->delivery_tracker)) {
        return false;
    }
    size_t queued = (c->output.length > c->output_offset) ?
                    (c->output.length - c->output_offset) : 0;
    if (queued >= CHAT_DELIVERY_OUTPUT_HIGH_WATER_BYTES) {
        return false;
    }
    if (c->output_offset != 0) {
        if (buffer_consume(&c->output, c->output_offset) != BUFFER_OK) {
            return false;
        }
        c->output_offset = 0;
    }
    if (buffer_append(&c->output, frame_buf->data, frame_buf->length) != BUFFER_OK) {
        return false;
    }
    delivery_tracker_add(&c->delivery_tracker, message_id, scope, 1);
    if (c->output_last_progress_ms == 0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            c->output_last_progress_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        }
    }
    return true;
}

static void server_on_delivery_ack(void *user_data,
                                   const char *connection_id,
                                   uint64_t connection_generation,
                                   const char *scope,
                                   int64_t message_id)
{
    ServerNotificationContext *ctx = (ServerNotificationContext *)user_data;
    if (ctx == NULL || ctx->connections == NULL || ctx->connection_count == NULL ||
        connection_id == NULL || scope == NULL || message_id <= 0) {
        return;
    }
    for (size_t i = 0; i < *ctx->connection_count; ++i) {
        Connection *c = &ctx->connections[i];
        if (c->fd < 0 || c->generation != connection_generation) {
            continue;
        }
        char cid[32];
        snprintf(cid, sizeof(cid), "conn-%d", c->fd);
        if (strcmp(cid, connection_id) == 0) {
            delivery_tracker_on_ack(&c->delivery_tracker, message_id, scope);
            return;
        }
    }
}

static const char *resolve_env_or_default(const char *env_name, const char *default_val)
{
    const char *val = getenv(env_name);
    if (val == NULL || val[0] == '\0') {
        return default_val;
    }
    return val;
}

static bool resolve_port_env(const char *env_name, uint16_t default_port, uint16_t *out_port)
{
    const char *val = getenv(env_name);
    if (val == NULL || val[0] == '\0') {
        *out_port = default_port;
        return true;
    }
    char *endptr = NULL;
    errno = 0;
    long parsed = strtol(val, &endptr, 10);
    if (errno != 0 || endptr == val || *endptr != '\0' || parsed < 0 || parsed > 65535) {
        return false;
    }
    *out_port = (uint16_t)parsed;
    return true;
}

int main(void)
{
    const char *bind_host = resolve_env_or_default("CHAT_HOST", CHAT_DEFAULT_HOST);
    uint16_t bind_port = 0;
    if (!resolve_port_env("CHAT_PORT", CHAT_DEFAULT_PORT, &bind_port)) {
        fputs("Invalid CHAT_PORT environment value (expected 0..65535)\n", stderr);
        return EXIT_FAILURE;
    }
    const char *db_path = resolve_env_or_default("CHAT_DB_PATH", CHAT_DEFAULT_DB_PATH);
    const char *log_path = resolve_env_or_default("CHAT_LOG_PATH", CHAT_DEFAULT_LOG_PATH);

    puts("TCP Chat Server");
    printf("Configured address: %s:%u\n", bind_host, (unsigned)bind_port);

    logger_global_init(log_path);
    log_info("server.start", "SUCCESS", NULL, "none", "network", bind_host);

    int listener_fd = listener_open(bind_host, bind_port, CHAT_LISTEN_BACKLOG);
    if (listener_fd == -1) {
        perror("Cannot open listening socket");
        log_error("server.start", "FAILURE", NULL, "none", "socket", bind_host, "EADDRINUSE");
        logger_global_close();
        return EXIT_FAILURE;
    }

    uint16_t actual_port = bind_port;
    struct sockaddr_in bound_addr = {0};
    socklen_t bound_len = sizeof(bound_addr);
    if (getsockname(listener_fd, (struct sockaddr *)&bound_addr, &bound_len) == 0) {
        actual_port = ntohs(bound_addr.sin_port);
    }

    struct sigaction stop_action = {0};
    stop_action.sa_handler = request_stop;
    if (sigemptyset(&stop_action.sa_mask) == -1 ||
        sigaction(SIGINT, &stop_action, NULL) == -1 ||
        sigaction(SIGTERM, &stop_action, NULL) == -1) {
        perror("Cannot install shutdown handler");
        close(listener_fd);
        logger_global_close();
        return EXIT_FAILURE;
    }

    int exit_status = EXIT_SUCCESS;

    Database db = {0};
    DbResult db_res = db_open(db_path, &db);
    if (db_res != DB_OK) {
        fprintf(stderr, "Cannot open database '%s': %s\n",
                db_path, db_result_name(db_res));
        log_error("db.open", "FAILURE", NULL, "none", "database", db_path, db_result_name(db_res));
        close(listener_fd);
        logger_global_close();
        return EXIT_FAILURE;
    }

    printf("Listening on %s:%u\n", bind_host, (unsigned)actual_port);
    puts("Listening for TCP connections; press Ctrl+C to stop.");
    fflush(stdout);

    SessionManager session_mgr;
    session_manager_init(&session_mgr);

    /* Mỗi slot đang dùng có một fd và input buffer; pollfds được dựng lại mỗi lượt. */
    Connection connections[CHAT_MAX_CONNECTIONS] = {0};
    size_t connection_count = 0;
    struct pollfd pollfds[CHAT_MAX_CONNECTIONS + 1] = {0};

    ServerNotificationContext notif_ctx = {
        .connections = connections,
        .connection_count = &connection_count,
        .session_mgr = &session_mgr
    };
    router_set_notification_dispatcher(server_dispatch_notification, &notif_ctx);
    router_set_delivery_hooks(server_dispatch_delivery_push, server_on_delivery_ack, &notif_ctx);

    DeliverySchedulerContext delivery_sched_ctx = {
        .connections = connections,
        .connection_count = &connection_count,
        .session_mgr = &session_mgr,
        .db = &db
    };

    while (!stop_requested) {
        pollfds[0] = (struct pollfd){.fd = listener_fd, .events = POLLIN};
        bool pending_frame = false;
        for (size_t index = 0; index < connection_count; ++index) {
            pollfds[index + 1] = (struct pollfd){.fd = connections[index].fd,
                                                .events = connections[index].peer_eof ||
                                                          connections[index].close_after_flush ?
                                                          0 : POLLIN};
            /* Chỉ theo dõi POLLOUT khi có byte chờ gửi, tránh busy loop. */
            if (connections[index].output.length > connections[index].output_offset) {
                pollfds[index + 1].events |= POLLOUT;
            }
            pending_frame |= connections[index].pending_frame ||
                             connections[index].delivery_fatal_timeout;
        }

        /* Frame còn sẵn trong RAM phải được xử lý ngay, không đợi thêm byte TCP. */
        int ready_count = poll(pollfds, connection_count + 1, pending_frame ? 0 : 1000);
        if (ready_count == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("Cannot poll listening socket");
            exit_status = EXIT_FAILURE;
            break;
        }
        if (pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fputs("Listening socket failed\n", stderr);
            exit_status = EXIT_FAILURE;
            break;
        }

        /* Duyệt ngược để swap-remove client đã đóng mà không lệch chỉ số poll. */
        for (size_t cursor = connection_count; cursor > 0; --cursor) {
            size_t index = cursor - 1;
            short events = pollfds[index + 1].revents;
            bool output_timed_out = connection_output_timed_out(&connections[index]);
            bool close_timed_out = connection_close_timed_out(&connections[index]);
            bool heartbeat_timed_out = connection_heartbeat_timed_out(&connections[index]);
            bool fatal_ack_timed_out = connection_delivery_fatal_timed_out(&connections[index]);
            if (!(events & (POLLIN | POLLOUT | POLLHUP | POLLERR | POLLNVAL)) &&
                !connections[index].pending_frame && !output_timed_out &&
                !close_timed_out && !heartbeat_timed_out && !fatal_ack_timed_out &&
                !(connections[index].close_after_flush &&
                  connections[index].output.length == connections[index].output_offset)) {
                continue;
            }

            size_t frames_processed = 0;
            ConnectionReceiveResult result = CONNECTION_RECEIVE_OK;
            if (close_timed_out) {
                result = CONNECTION_RECEIVE_CLOSE_AFTER_FLUSH;
            } else if (heartbeat_timed_out) {
                result = CONNECTION_RECEIVE_HEARTBEAT_TIMEOUT;
            } else if (output_timed_out) {
                result = CONNECTION_RECEIVE_OUTPUT_TIMEOUT;
            } else if (fatal_ack_timed_out) {
                result = CONNECTION_RECEIVE_DELIVERY_ACK_TIMEOUT;
            } else if (events & (POLLERR | POLLNVAL)) {
                result = CONNECTION_RECEIVE_IO_ERROR;
            } else {
                /* Xả output cũ trước khi xếp thêm frame để giảm nguy cơ đầy queue. */
                if ((events & POLLOUT) && !connection_flush(&connections[index])) {
                    result = CONNECTION_RECEIVE_IO_ERROR;
                }
                if (result == CONNECTION_RECEIVE_OK &&
                    !connections[index].peer_eof && !connections[index].close_after_flush &&
                    ((events & (POLLIN | POLLHUP)) || connections[index].pending_frame)) {
                    result = connection_receive(&connections[index], &frames_processed);
                }
            }
            if (frames_processed != 0) {
                printf("Received %zu complete frame(s) from a client.\n", frames_processed);
            }
            /* Peer ngừng gửi không có nghĩa peer đã ngừng đọc: xả hết response rồi đóng. */
            if (result == CONNECTION_RECEIVE_EOF &&
                connections[index].output.length > connections[index].output_offset) {
                continue;
            }
            if (result == CONNECTION_RECEIVE_OK && connections[index].peer_eof &&
                connections[index].output.length == connections[index].output_offset) {
                result = CONNECTION_RECEIVE_EOF;
            }
            if (result == CONNECTION_RECEIVE_OK && connections[index].close_after_flush &&
                connections[index].output.length == connections[index].output_offset) {
                result = CONNECTION_RECEIVE_CLOSE_AFTER_FLUSH;
            }
            if (result == CONNECTION_RECEIVE_OK) {
                if (connection_needs_heartbeat_ping(&connections[index])) {
                    char nonce[CHAT_HEARTBEAT_NONCE_HEX_LEN + 1];
                    struct timespec now_ts;
                    if (clock_gettime(CLOCK_MONOTONIC, &now_ts) == 0 &&
                        protocol_generate_nonce_hex(nonce, sizeof(nonce))) {
                        int64_t now_ms = (int64_t)now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
                        if (connection_queue_heartbeat_ping(
                                &connections[index], nonce,
                                now_ms + CHAT_HEARTBEAT_PONG_TIMEOUT_MS)) {
                            puts("Idle connection; queued heartbeat.ping.");
                        }
                    }
                }
                continue;
            }

            char conn_id_str[32];
            snprintf(conn_id_str, sizeof(conn_id_str), "conn-%d", connections[index].fd);
            log_info("transport.disconnect", "SUCCESS", NULL, conn_id_str, "connection", disconnect_reason(result));
            printf("Client disconnected (%s).\n", disconnect_reason(result));
            connection_destroy(&connections[index]);
            --connection_count;
            if (index != connection_count) {
                connections[index] = connections[connection_count];
            }
            connections[connection_count] = (Connection){0};
        }

        if (pollfds[0].revents & POLLIN) {
            /* Mỗi lượt xử lý tối đa 16 kết nối đang chờ, không phải 16 client tối đa. */
            for (int accepted_count = 0; accepted_count < 16; ++accepted_count) {
                struct sockaddr_in peer_address = {0};
                int client_fd = listener_accept(listener_fd, &peer_address);
                if (client_fd == -1) {
                    /* Hàng đợi đã rỗng: trở lại poll(), không xem là lỗi. */
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EINTR || errno == ECONNABORTED) {
                        continue;
                    }
                    perror("Cannot accept TCP connection");
                    exit_status = EXIT_FAILURE;
                    stop_requested = 1;
                    break;
                }
                if (connection_count == CHAT_MAX_CONNECTIONS) {
                    puts("Connection limit reached; closing new client.");
                    close(client_fd);
                    continue;
                }

                if (connection_init(&connections[connection_count], client_fd) != BUFFER_OK) {
                    fputs("Cannot initialize client input buffer\n", stderr);
                    close(client_fd);
                    continue;
                }
                connection_set_db(&connections[connection_count], &db);
                connection_set_session_manager(&connections[connection_count], &session_mgr);
                if (!connection_queue_ready(&connections[connection_count])) {
                    fputs("Cannot queue connection.ready event\n", stderr);
                    connection_destroy(&connections[connection_count]);
                    continue;
                }
                ++connection_count;
                char conn_id_str[32];
                snprintf(conn_id_str, sizeof(conn_id_str), "conn-%d", client_fd);
                log_info("transport.accept", "SUCCESS", NULL, conn_id_str, "connection", "ready");
                puts("Accepted TCP client; connection.ready queued.");
            }
        }

        /* Quét và giao các tin nhắn pending và retry cho tất cả người dùng online */
        delivery_scheduler_tick(&delivery_sched_ctx);
    }

    router_set_delivery_hooks(NULL, NULL, NULL);
    router_set_notification_dispatcher(NULL, NULL);

    for (size_t index = 0; index < connection_count; ++index) {
        connection_destroy(&connections[index]);
    }
    db_close(&db);
    if (close(listener_fd) == -1) {
        perror("Cannot close listening socket");
        exit_status = EXIT_FAILURE;
    }
    log_info("server.stop", "SUCCESS", NULL, "none", "server", "shutdown");
    logger_global_close();
    puts("Server stopped.");

    return exit_status;
}
