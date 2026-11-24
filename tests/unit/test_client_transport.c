#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "client/transport.h"
#include "shared/frame.h"
#include "shared/protocol.h"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static int open_test_listener(uint16_t *port)
{
    int listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    check(listener_fd >= 0, "create fake server socket");
    struct sockaddr_in address = {.sin_family = AF_INET};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(listener_fd, (struct sockaddr *)&address, sizeof(address)) == 0,
          "bind fake server to a free loopback port");
    check(listen(listener_fd, 1) == 0, "listen for test client");
    socklen_t address_size = sizeof(address);
    check(getsockname(listener_fd, (struct sockaddr *)&address, &address_size) == 0,
          "read assigned test port");
    *port = ntohs(address.sin_port);
    return listener_fd;
}

typedef enum {
    FAKE_NORMAL = 0,
    FAKE_TRICKLE_RESPONSE,
    FAKE_BAD_READY,
    FAKE_HEARTBEAT_THEN_REPLY
} FakeServerMode;

static void serve_one_client(int listener_fd, FakeServerMode mode)
{
    int peer_fd = accept(listener_fd, NULL, NULL);
    if (peer_fd == -1) {
        _exit(EXIT_FAILURE);
    }
    ByteBuffer ready = {0};
    const char bad_ready[] = "{\"v\":1,\"kind\":\"event\",\"type\":\"connection.ready\","
                             "\"data\":{\"protocol_version\":2,\"max_frame_bytes\":65536,"
                             "\"server_time\":1}}";
    if (buffer_init(&ready, 256) != BUFFER_OK) {
        _exit(EXIT_FAILURE);
    }
    bool ready_ok = mode == FAKE_BAD_READY ?
                    frame_encode(&ready, bad_ready, strlen(bad_ready)) == FRAME_OK :
                    protocol_encode_ready_event(1, &ready) == PROTOCOL_OK;
    if (!ready_ok) {
        _exit(EXIT_FAILURE);
    }
    size_t ready_sent = 0;
    while (ready_sent < ready.length) {
        ssize_t sent = send(peer_fd, ready.data + ready_sent, ready.length - ready_sent,
                            MSG_NOSIGNAL);
        if (sent <= 0) {
            _exit(EXIT_FAILURE);
        }
        ready_sent += (size_t)sent;
    }
    buffer_destroy(&ready);
    if (mode == FAKE_BAD_READY) {
        close(peer_fd);
        close(listener_fd);
        _exit(EXIT_SUCCESS);
    }
    /* Fake server chỉ phản hồi sau khi client đã chờ ready và gửi request. */
    const unsigned char expected_request[] = {0, 0, 0, 2, 'h', 'i'};
    unsigned char request[sizeof(expected_request)];
    size_t request_length = 0;
    while (request_length < sizeof(request)) {
        ssize_t received = recv(peer_fd, request + request_length,
                                 sizeof(request) - request_length, 0);
        if (received <= 0) {
            _exit(EXIT_FAILURE);
        }
        request_length += (size_t)received;
    }
    if (memcmp(request, expected_request, sizeof(request)) != 0) {
        _exit(EXIT_FAILURE);
    }

    if (mode == FAKE_HEARTBEAT_THEN_REPLY) {
        ByteBuffer ping_frame = {0};
        const char test_nonce[] = "1234567890abcdef1234567890abcdef";
        if (buffer_init(&ping_frame, 256) != BUFFER_OK ||
            protocol_encode_heartbeat_ping(test_nonce, &ping_frame) != PROTOCOL_OK) {
            _exit(EXIT_FAILURE);
        }
        size_t ping_sent = 0;
        while (ping_sent < ping_frame.length) {
            ssize_t sent = send(peer_fd, ping_frame.data + ping_sent,
                                ping_frame.length - ping_sent, MSG_NOSIGNAL);
            if (sent <= 0) {
                _exit(EXIT_FAILURE);
            }
            ping_sent += (size_t)sent;
        }
        buffer_destroy(&ping_frame);

        /* Đọc control pong từ client */
        ByteBuffer client_input = {0};
        if (buffer_init(&client_input, 256) != BUFFER_OK) {
            _exit(EXIT_FAILURE);
        }
        char pong_nonce[33] = {0};
        bool pong_received = false;
        while (!pong_received) {
            FrameView view = {0};
            if (frame_peek(&client_input, &view) == FRAME_OK) {
                if (protocol_parse_heartbeat_pong(view.body, view.length, pong_nonce) == PROTOCOL_OK &&
                    strcmp(pong_nonce, test_nonce) == 0) {
                    pong_received = true;
                    frame_consume(&client_input);
                    break;
                }
                _exit(EXIT_FAILURE);
            }
            unsigned char buf[128];
            ssize_t n = recv(peer_fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                _exit(EXIT_FAILURE);
            }
            if (buffer_append(&client_input, buf, (size_t)n) != BUFFER_OK) {
                _exit(EXIT_FAILURE);
            }
        }
        buffer_destroy(&client_input);
    }

    const unsigned char reply[] = {0, 0, 0, 2, 'o', 'k'};
    for (size_t index = 0; index < sizeof(reply); ++index) {
        if (send(peer_fd, reply + index, 1, MSG_NOSIGNAL) != 1) {
            break;
        }
        if (mode == FAKE_TRICKLE_RESPONSE) {
            /* Mỗi byte đến trước 150 ms, nhưng cả frame mất hơn 150 ms. */
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 60000000};
            nanosleep(&pause, NULL);
        }
    }
    close(peer_fd);
    close(listener_fd);
    _exit(EXIT_SUCCESS);
}

static void test_roundtrip(bool trickle)
{
    uint16_t port = 0;
    int listener_fd = open_test_listener(&port);
    pid_t server_pid = fork();
    check(server_pid >= 0, "start fake server process");
    if (server_pid == 0) {
        serve_one_client(listener_fd, trickle ? FAKE_TRICKLE_RESPONSE : FAKE_NORMAL);
    }
    close(listener_fd);

    ByteBuffer reply = {0};
    ClientTransportResult result = client_roundtrip_to("127.0.0.1", port, "hi", 2,
                                                        trickle ? 150 : 1000, &reply);
    if (trickle) {
        check(result == CLIENT_TRANSPORT_TIMEOUT,
              "one deadline rejects a response whose bytes trickle indefinitely");
        check(reply.data == NULL, "timeout leaves no partial reply allocated");
    } else {
        check(result == CLIENT_TRANSPORT_OK && reply.length == 2 &&
                  memcmp(reply.data, "ok", 2) == 0,
              "complete response arrives before the deadline");
    }
    buffer_destroy(&reply);

    int child_status = 0;
    check(waitpid(server_pid, &child_status, 0) == server_pid &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == EXIT_SUCCESS,
          "fake server exits cleanly");
}

static void test_invalid_ready(void)
{
    uint16_t port = 0;
    int listener_fd = open_test_listener(&port);
    pid_t server_pid = fork();
    check(server_pid >= 0, "start fake server with unsupported ready version");
    if (server_pid == 0) {
        serve_one_client(listener_fd, FAKE_BAD_READY);
    }
    close(listener_fd);
    ByteBuffer reply = {0};
    check(client_roundtrip_to("127.0.0.1", port, "hi", 2, 1000, &reply) ==
              CLIENT_TRANSPORT_PROTOCOL_ERROR && reply.data == NULL,
          "client rejects bad ready before sending request");
    int child_status = 0;
    check(waitpid(server_pid, &child_status, 0) == server_pid &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == EXIT_SUCCESS,
          "bad-ready fake server exits cleanly");
}

static void test_client_auto_heartbeat_pong(void)
{
    uint16_t port = 0;
    int listener_fd = open_test_listener(&port);
    pid_t server_pid = fork();
    check(server_pid >= 0, "start fake server with interleaved heartbeat");
    if (server_pid == 0) {
        serve_one_client(listener_fd, FAKE_HEARTBEAT_THEN_REPLY);
    }
    close(listener_fd);

    ByteBuffer reply = {0};
    ClientTransportResult result = client_roundtrip_to("127.0.0.1", port, "hi", 2,
                                                        1000, &reply);
    check(result == CLIENT_TRANSPORT_OK && reply.length == 2 &&
              memcmp(reply.data, "ok", 2) == 0,
          "client automatically answers heartbeat.pong and still receives business reply");
    buffer_destroy(&reply);

    int child_status = 0;
    check(waitpid(server_pid, &child_status, 0) == server_pid &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == EXIT_SUCCESS,
          "heartbeat fake server exits cleanly");
}

static void serve_interactive_client(int listener_fd)
{
    int peer_fd = accept(listener_fd, NULL, NULL);
    if (peer_fd == -1) {
        _exit(EXIT_FAILURE);
    }

    /* 1. Gửi connection.ready */
    ByteBuffer ready = {0};
    if (buffer_init(&ready, 256) != BUFFER_OK ||
        protocol_encode_ready_event(1, &ready) != PROTOCOL_OK) {
        _exit(EXIT_FAILURE);
    }
    if (send(peer_fd, ready.data, ready.length, MSG_NOSIGNAL) <= 0) {
        _exit(EXIT_FAILURE);
    }
    buffer_destroy(&ready);

    ByteBuffer input = {0};
    if (buffer_init(&input, 2048) != BUFFER_OK) {
        _exit(EXIT_FAILURE);
    }

    bool got_ping = false;
    bool got_pong = false;
    bool got_close = false;
    const char nonce[] = "1234567890abcdef1234567890abcdef";

    while (!got_close) {
        FrameView view = {0};
        if (frame_peek(&input, &view) == FRAME_OK) {
            char parsed_nonce[33] = {0};
            if (protocol_parse_heartbeat_pong(view.body, view.length, parsed_nonce) == PROTOCOL_OK) {
                if (strcmp(parsed_nonce, nonce) == 0) {
                    got_pong = true;
                }
                frame_consume(&input);
                continue;
            }

            ProtocolRequest req = {0};
            if (protocol_parse_request(view.body, view.length, &req) == PROTOCOL_OK) {
                if (strcmp(req.type, "system.ping") == 0) {
                    got_ping = true;
                    ByteBuffer resp = {0};
                    if (buffer_init(&resp, 256) == BUFFER_OK &&
                        protocol_encode_ping_response(&req, 1792054800000LL, &resp) == PROTOCOL_OK) {
                        send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                    }
                    buffer_destroy(&resp);

                    /* Gửi event heartbeat.ping tới client */
                    ByteBuffer hb_ping = {0};
                    if (buffer_init(&hb_ping, 256) == BUFFER_OK &&
                        protocol_encode_heartbeat_ping(nonce, &hb_ping) == PROTOCOL_OK) {
                        send(peer_fd, hb_ping.data, hb_ping.length, MSG_NOSIGNAL);
                    }
                    buffer_destroy(&hb_ping);
                } else if (strcmp(req.type, "connection.close") == 0) {
                    got_close = true;
                    ByteBuffer resp = {0};
                    if (buffer_init(&resp, 256) == BUFFER_OK &&
                        protocol_encode_close_response(&req, &resp) == PROTOCOL_OK) {
                        send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                    }
                    buffer_destroy(&resp);
                }
                protocol_request_destroy(&req);
                frame_consume(&input);
                continue;
            }
            frame_consume(&input);
            continue;
        }

        unsigned char buf[512];
        ssize_t n = recv(peer_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        buffer_append(&input, buf, (size_t)n);
    }

    /* Nhận pong nếu client gửi sau close request */
    while (!got_pong) {
        FrameView view = {0};
        if (frame_peek(&input, &view) == FRAME_OK) {
            char parsed_nonce[33] = {0};
            if (protocol_parse_heartbeat_pong(view.body, view.length, parsed_nonce) == PROTOCOL_OK &&
                strcmp(parsed_nonce, nonce) == 0) {
                got_pong = true;
            }
            frame_consume(&input);
            continue;
        }
        unsigned char buf[256];
        ssize_t n = recv(peer_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        buffer_append(&input, buf, (size_t)n);
    }

    buffer_destroy(&input);
    close(peer_fd);
    close(listener_fd);
    if (!got_ping || !got_pong || !got_close) {
        _exit(EXIT_FAILURE);
    }
    _exit(EXIT_SUCCESS);
}

static void test_interactive_session(void)
{
    uint16_t port = 0;
    int listener_fd = open_test_listener(&port);

    int pipe_in[2];
    int pipe_out[2];
    check(pipe(pipe_in) == 0 && pipe(pipe_out) == 0, "create stdin and stdout test pipes");

    pid_t server_pid = fork();
    check(server_pid >= 0, "start fake server for interactive session");
    if (server_pid == 0) {
        close(pipe_in[0]);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_out[1]);
        serve_interactive_client(listener_fd);
    }
    close(listener_fd);

    /* Parent ghi các lệnh vào pipe_in */
    const char commands[] = "/ping\n/close\n";
    check(write(pipe_in[1], commands, strlen(commands)) == (ssize_t)strlen(commands),
          "feed commands to client stdin");
    close(pipe_in[1]); /* Đóng chiều ghi để client nhận EOF sau /close nếu cần */

    /* Chạy client interactive loop */
    ClientTransportResult res = client_interactive_loop("127.0.0.1", port,
                                                        pipe_in[0], pipe_out[1]);
    check(res == CLIENT_TRANSPORT_OK, "interactive loop finishes successfully");
    close(pipe_in[0]);
    close(pipe_out[1]);

    /* Đọc toàn bộ output để kiểm tra */
    char output_buf[2048] = {0};
    ssize_t out_len = read(pipe_out[0], output_buf, sizeof(output_buf) - 1);
    check(out_len > 0, "read interactive client stdout output");
    output_buf[out_len] = '\0';
    close(pipe_out[0]);

    check(strstr(output_buf, "protocol v1 ready") != NULL, "logged ready received");
    check(strstr(output_buf, "system.ping") != NULL, "sent ping request");
    check(strstr(output_buf, "[Heartbeat] Responded pong") != NULL,
          "handled background heartbeat.ping automatically");
    check(strstr(output_buf, "connection.close") != NULL, "sent close request");
    check(strstr(output_buf, "\"closing\":true") != NULL, "received close response confirmation");

    int child_status = 0;
    check(waitpid(server_pid, &child_status, 0) == server_pid &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == EXIT_SUCCESS,
          "interactive fake server exits cleanly");
}

static void serve_messaging_client(int listener_fd)
{
    int peer_fd = accept(listener_fd, NULL, NULL);
    if (peer_fd == -1) {
        _exit(EXIT_FAILURE);
    }

    /* 1. Gửi connection.ready */
    ByteBuffer ready = {0};
    if (buffer_init(&ready, 256) != BUFFER_OK ||
        protocol_encode_ready_event(1, &ready) != PROTOCOL_OK) {
        _exit(EXIT_FAILURE);
    }
    if (send(peer_fd, ready.data, ready.length, MSG_NOSIGNAL) <= 0) {
        _exit(EXIT_FAILURE);
    }
    buffer_destroy(&ready);

    ByteBuffer input = {0};
    if (buffer_init(&input, 4096) != BUFFER_OK) {
        _exit(EXIT_FAILURE);
    }

    bool got_send = false;
    bool got_ack1 = false;
    bool got_chat_send = false;
    bool got_close = false;

    while (!got_close || !got_send || !got_ack1 || !got_chat_send) {
        FrameView view = {0};
        if (frame_peek(&input, &view) == FRAME_OK) {
            ProtocolRequest req = {0};
            if (protocol_parse_request(view.body, view.length, &req) == PROTOCOL_OK) {
                if (strcmp(req.type, "direct.send") == 0) {
                    if (!got_send) {
                        got_send = true;
                        /* Trả response direct.send */
                        ByteBuffer resp = {0};
                        buffer_init(&resp, 512);
                        protocol_encode_direct_send_response(&req, "direct", 101, "0123456789abcdef0123456789abcdef", 1792054800000LL, "stored", &resp);
                        send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                        buffer_destroy(&resp);

                        /* Gửi direct.message push event (Bob gửi Alice) */
                        ByteBuffer dm_evt = {0};
                        buffer_init(&dm_evt, 512);
                        protocol_encode_direct_message_event(201, "abcdef0123456789abcdef0123456789", 2, "bob", 1, "Chao ban Alice!", 1792054801000LL, &dm_evt);
                        send(peer_fd, dm_evt.data, dm_evt.length, MSG_NOSIGNAL);
                        buffer_destroy(&dm_evt);
                    } else {
                        got_chat_send = true;
                        /* Trả response cho chat mode direct.send */
                        ByteBuffer resp = {0};
                        buffer_init(&resp, 512);
                        protocol_encode_direct_send_response(&req, "direct", 102, "0123456789abcdef0123456789abcdef", 1792054802000LL, "stored", &resp);
                        send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                        buffer_destroy(&resp);
                    }
                } else if (strcmp(req.type, "delivery.ack") == 0) {
                    got_ack1 = true;
                    /* Phản hồi delivery.ack response */
                    ProtocolAckItem aitem = {.scope = "direct", .message_id = 201};
                    ByteBuffer ack_resp = {0};
                    buffer_init(&ack_resp, 256);
                    protocol_encode_delivery_ack_response(&req, &aitem, 1, &ack_resp);
                    send(peer_fd, ack_resp.data, ack_resp.length, MSG_NOSIGNAL);
                    buffer_destroy(&ack_resp);
                } else if (strcmp(req.type, "connection.close") == 0) {
                    got_close = true;
                    ByteBuffer resp = {0};
                    if (buffer_init(&resp, 256) == BUFFER_OK &&
                        protocol_encode_close_response(&req, &resp) == PROTOCOL_OK) {
                        send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                    }
                    buffer_destroy(&resp);
                }
                protocol_request_destroy(&req);
            }
            frame_consume(&input);
            continue;
        }

        unsigned char buf[512];
        ssize_t n = recv(peer_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        buffer_append(&input, buf, (size_t)n);
    }

    buffer_destroy(&input);
    close(peer_fd);
    close(listener_fd);
    if (!got_send || !got_ack1 || !got_chat_send || !got_close) {
        _exit(EXIT_FAILURE);
    }
    _exit(EXIT_SUCCESS);
}

static void test_interactive_messaging_and_auto_ack(void)
{
    uint16_t port = 0;
    int listener_fd = open_test_listener(&port);

    int pipe_in[2];
    int pipe_out[2];
    check(pipe(pipe_in) == 0 && pipe(pipe_out) == 0, "create stdin and stdout pipes for messaging test");

    pid_t server_pid = fork();
    check(server_pid >= 0, "start fake server for messaging test");
    if (server_pid == 0) {
        close(pipe_in[0]);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_out[1]);
        serve_messaging_client(listener_fd);
    }
    close(listener_fd);

    /* Client gửi các lệnh */
    const char commands[] =
        "/send 2 Xin chao\n"
        "/chat 2\n"
        "Hello from chat mode\n"
        "/back\n"
        "/close\n";
    check(write(pipe_in[1], commands, strlen(commands)) == (ssize_t)strlen(commands),
          "feed messaging commands to client");
    close(pipe_in[1]);

    ClientTransportResult res = client_interactive_loop("127.0.0.1", port,
                                                        pipe_in[0], pipe_out[1]);
    check(res == CLIENT_TRANSPORT_OK, "client messaging session finishes ok");
    close(pipe_in[0]);
    close(pipe_out[1]);

    char output_buf[4096] = {0};
    ssize_t out_len = read(pipe_out[0], output_buf, sizeof(output_buf) - 1);
    check(out_len > 0, "read client messaging output");
    output_buf[out_len] = '\0';
    close(pipe_out[0]);

    check(strstr(output_buf, "[Message from bob (2)]: Chao ban Alice!") != NULL,
          "displayed received direct.message");
    check(strstr(output_buf, "[Auto-ACK] Sent delivery.ack for message_id 201") != NULL,
          "sent auto delivery.ack");
    check(strstr(output_buf, "Entered chat mode with user 2") != NULL, "entered chat mode");
    check(strstr(output_buf, "Exited chat mode with user 2") != NULL, "exited chat mode");

    int child_status = 0;
    check(waitpid(server_pid, &child_status, 0) == server_pid &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == EXIT_SUCCESS,
          "messaging fake server exits cleanly");
}

static void serve_group_messaging_client(int listener_fd)
{
    int peer_fd = accept(listener_fd, NULL, NULL);
    if (peer_fd == -1) {
        _exit(EXIT_FAILURE);
    }

    ByteBuffer ready = {0};
    if (buffer_init(&ready, 256) != BUFFER_OK ||
        protocol_encode_ready_event(1, &ready) != PROTOCOL_OK) {
        _exit(EXIT_FAILURE);
    }
    send(peer_fd, ready.data, ready.length, MSG_NOSIGNAL);
    buffer_destroy(&ready);

    ByteBuffer input = {0};
    if (buffer_init(&input, 4096) != BUFFER_OK) {
        _exit(EXIT_FAILURE);
    }

    bool got_create = false;
    bool got_send = false;
    bool got_ack1 = false;
    bool got_chat_send = false;
    bool got_close = false;

    while (!got_close || !got_ack1) {
        FrameView view = {0};
        if (frame_peek(&input, &view) == FRAME_OK) {
            ProtocolRequest req = {0};
            if (protocol_parse_request(view.body, view.length, &req) == PROTOCOL_OK) {
                if (strcmp(req.type, "group.create") == 0) {
                    got_create = true;
                    ByteBuffer resp = {0};
                    buffer_init(&resp, 512);
                    ProtocolGroup pg = {
                        .group_id = 10,
                        .name = "Alpha",
                        .visibility = "public",
                        .owner_user_id = 1,
                        .member_count = 1,
                        .created_at = 1000
                    };
                    protocol_encode_group_response(&req, &pg, &resp);
                    send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                    buffer_destroy(&resp);
                } else if (strcmp(req.type, "group.send") == 0) {
                    if (!got_send) {
                        got_send = true;
                        ByteBuffer resp = {0};
                        buffer_init(&resp, 512);
                        protocol_encode_group_send_response(&req, "group", 300, "11111111111111111111111111111111", 1001, "stored", &resp);
                        send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                        buffer_destroy(&resp);

                        /* Server push group.message event */
                        ByteBuffer grp_evt = {0};
                        buffer_init(&grp_evt, 512);
                        protocol_encode_group_message_event(301, "22222222222222222222222222222222", 10, "Alpha", 2, "bob", "Hello ca nhom!", 1002, &grp_evt);
                        send(peer_fd, grp_evt.data, grp_evt.length, MSG_NOSIGNAL);
                        buffer_destroy(&grp_evt);
                    } else {
                        got_chat_send = true;
                        ByteBuffer resp = {0};
                        buffer_init(&resp, 512);
                        protocol_encode_group_send_response(&req, "group", 302, "33333333333333333333333333333333", 1003, "stored", &resp);
                        send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                        buffer_destroy(&resp);
                    }
                } else if (strcmp(req.type, "delivery.ack") == 0) {
                    got_ack1 = true;
                    ProtocolAckItem aitem = {.scope = "group", .message_id = 301};
                    ByteBuffer ack_resp = {0};
                    buffer_init(&ack_resp, 256);
                    protocol_encode_delivery_ack_response(&req, &aitem, 1, &ack_resp);
                    send(peer_fd, ack_resp.data, ack_resp.length, MSG_NOSIGNAL);
                    buffer_destroy(&ack_resp);
                } else if (strcmp(req.type, "connection.close") == 0) {
                    got_close = true;
                    ByteBuffer resp = {0};
                    if (buffer_init(&resp, 256) == BUFFER_OK &&
                        protocol_encode_close_response(&req, &resp) == PROTOCOL_OK) {
                        send(peer_fd, resp.data, resp.length, MSG_NOSIGNAL);
                    }
                    buffer_destroy(&resp);
                }
                protocol_request_destroy(&req);
            }
            frame_consume(&input);
            continue;
        }

        unsigned char buf[512];
        ssize_t n = recv(peer_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        buffer_append(&input, buf, (size_t)n);
    }

    buffer_destroy(&input);
    close(peer_fd);
    close(listener_fd);
    if (!got_create || !got_send || !got_ack1 || !got_chat_send || !got_close) {
        _exit(EXIT_FAILURE);
    }
    _exit(EXIT_SUCCESS);
}

static void test_interactive_group_messaging_and_auto_ack(void)
{
    uint16_t port = 0;
    int listener_fd = open_test_listener(&port);

    int pipe_in[2];
    int pipe_out[2];
    check(pipe(pipe_in) == 0 && pipe(pipe_out) == 0, "create stdin and stdout pipes for group messaging test");

    pid_t server_pid = fork();
    check(server_pid >= 0, "start fake server for group messaging test");
    if (server_pid == 0) {
        close(pipe_in[0]);
        close(pipe_in[1]);
        close(pipe_out[0]);
        close(pipe_out[1]);
        serve_group_messaging_client(listener_fd);
    }
    close(listener_fd);

    /* Client gửi các lệnh nhóm */
    const char commands[] =
        "/group_create Alpha public\n"
        "/group_send 10 Xin chao ca nhom\n"
        "/group_chat 10\n"
        "Tin nhan tu group chat mode\n"
        "/back\n"
        "/close\n";
    check(write(pipe_in[1], commands, strlen(commands)) == (ssize_t)strlen(commands),
          "feed group messaging commands to client");
    close(pipe_in[1]);

    ClientTransportResult res = client_interactive_loop("127.0.0.1", port,
                                                        pipe_in[0], pipe_out[1]);
    check(res == CLIENT_TRANSPORT_OK, "client group messaging session finishes ok");
    close(pipe_in[0]);
    close(pipe_out[1]);

    char output_buf[4096] = {0};
    ssize_t out_len = read(pipe_out[0], output_buf, sizeof(output_buf) - 1);
    check(out_len > 0, "read client group messaging output");
    output_buf[out_len] = '\0';
    close(pipe_out[0]);

    check(strstr(output_buf, "[Group Alpha (10) from bob (2)]: Hello ca nhom!") != NULL,
          "displayed received group.message");
    check(strstr(output_buf, "[Auto-ACK] Sent delivery.ack for group message_id 301") != NULL,
          "sent auto delivery.ack for group");
    check(strstr(output_buf, "Entered group chat mode for group 10") != NULL, "entered group chat mode");
    check(strstr(output_buf, "Exited group chat mode for group 10") != NULL, "exited group chat mode");

    int child_status = 0;
    check(waitpid(server_pid, &child_status, 0) == server_pid &&
              WIFEXITED(child_status) && WEXITSTATUS(child_status) == EXIT_SUCCESS,
          "group messaging fake server exits cleanly");
}

int main(void)
{
    test_roundtrip(false);
    test_roundtrip(true);
    test_invalid_ready();
    test_client_auto_heartbeat_pong();
    test_interactive_session();
    test_interactive_messaging_and_auto_ack();
    test_interactive_group_messaging_and_auto_ack();
    puts("7 client transport tests passed.");
    return EXIT_SUCCESS;
}
