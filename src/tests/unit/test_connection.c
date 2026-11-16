/* Kiểm tra từng Connection bằng socketpair: byte tới từng phần, nhiều frame và EOF. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>

#include "server/net/connection.h"
#include "shared/frame.h"
#include "shared/limits.h"
#include "shared/protocol.h"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void send_bytes(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t sent = 0;
    while (sent < length) {
        ssize_t result = send(fd, bytes + sent, length - sent, MSG_NOSIGNAL);
        check(result > 0, "send test bytes into socketpair");
        sent += (size_t)result;
    }
}

static void open_test_connection(Connection *connection, int *peer_fd)
{
    int sockets[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                     0, sockets) == 0,
          "create nonblocking test socketpair");
    check(connection_init(connection, sockets[0]) == BUFFER_OK,
          "connection takes ownership of server-side socket");
    *peer_fd = sockets[1];
}

static void append_ping_frame(ByteBuffer *frames, unsigned request_number)
{
    char body[128];
    int length = snprintf(body, sizeof(body),
                          "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                          "\"request_id\":\"%u\",\"data\":{}}", request_number);
    check(length > 0 && (size_t)length < sizeof(body) &&
              frame_encode(frames, body, (size_t)length) == FRAME_OK,
          "encode system.ping test frame");
}

static void make_ping_frame(ByteBuffer *frame, unsigned request_number)
{
    check(buffer_init(frame, 256) == BUFFER_OK, "allocate system.ping test frame");
    append_ping_frame(frame, request_number);
}

static void check_response_frames(const ByteBuffer *output, unsigned expected_count)
{
    ByteBuffer copy = {0};
    check(buffer_init(&copy, output->length) == BUFFER_OK &&
              buffer_append(&copy, output->data, output->length) == BUFFER_OK,
          "copy queued response frames for inspection");
    for (unsigned number = 1; number <= expected_count; ++number) {
        FrameView view = {0};
        check(frame_peek(&copy, &view) == FRAME_OK,
              "queued response has a complete length-prefix frame");
        char json[256];
        check(view.length < sizeof(json), "ping response fits test buffer");
        memcpy(json, view.body, view.length);
        json[view.length] = '\0';
        cJSON *response = cJSON_Parse(json);
        check(response != NULL, "queued response body is valid JSON");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
        const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(response, "request_id");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
        const cJSON *server_time = cJSON_GetObjectItemCaseSensitive(data, "server_time");
        char expected_id[24];
        snprintf(expected_id, sizeof(expected_id), "%u", number);
        check(cJSON_IsString(type) && strcmp(type->valuestring, "system.ping") == 0 &&
                  cJSON_IsString(request_id) &&
                  strcmp(request_id->valuestring, expected_id) == 0 &&
                  cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")) &&
                  cJSON_IsNumber(server_time),
              "response preserves request order and contains server time");
        cJSON_Delete(response);
        check(frame_consume(&copy) == FRAME_OK, "consume inspected response frame");
    }
    check(copy.length == 0, "no extra response frames remain");
    buffer_destroy(&copy);
}

static void check_error_frame(const ByteBuffer *output, unsigned frame_index,
                              const char *kind, const char *type,
                              const char *request_id, const char *code)
{
    ByteBuffer copy = {0};
    check(buffer_init(&copy, output->length) == BUFFER_OK &&
              buffer_append(&copy, output->data, output->length) == BUFFER_OK,
          "copy output for error envelope inspection");
    for (unsigned index = 0; index < frame_index; ++index) {
        check(frame_consume(&copy) == FRAME_OK, "skip earlier queued frame");
    }
    FrameView view = {0};
    check(frame_peek(&copy, &view) == FRAME_OK, "error has a complete frame");
    char json[512];
    check(view.length < sizeof(json), "error fits JSON test buffer");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    cJSON *envelope = cJSON_Parse(json);
    check(envelope != NULL, "error is valid JSON");
    const cJSON *actual_kind = cJSON_GetObjectItemCaseSensitive(envelope, "kind");
    const cJSON *actual_type = cJSON_GetObjectItemCaseSensitive(envelope, "type");
    const cJSON *actual_id = cJSON_GetObjectItemCaseSensitive(envelope, "request_id");
    const cJSON *details = cJSON_GetObjectItemCaseSensitive(
        envelope, request_id == NULL ? "data" : "error");
    const cJSON *actual_code = cJSON_GetObjectItemCaseSensitive(details, "code");
    check(cJSON_IsString(actual_kind) && strcmp(actual_kind->valuestring, kind) == 0 &&
              cJSON_IsString(actual_type) && strcmp(actual_type->valuestring, type) == 0 &&
              ((request_id == NULL && actual_id == NULL) ||
               (cJSON_IsString(actual_id) &&
                strcmp(actual_id->valuestring, request_id) == 0)) &&
              cJSON_IsString(actual_code) && strcmp(actual_code->valuestring, code) == 0,
          "error envelope has expected kind, type, ID and code");
    if (request_id != NULL) {
        check(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(envelope, "ok")) &&
                  cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(details, "retryable")),
              "correlated error is an unsuccessful non-retryable response");
    }
    cJSON_Delete(envelope);
    buffer_destroy(&copy);
}

static int64_t test_monotonic_milliseconds(void)
{
    struct timespec now;
    check(clock_gettime(CLOCK_MONOTONIC, &now) == 0,
          "read monotonic clock for output deadline test");
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void test_fragmented_header_and_body(void)
{
    Connection connection = {0};
    int peer_fd;
    size_t frames_processed = 0;
    ByteBuffer frame = {0};

    open_test_connection(&connection, &peer_fd);
    make_ping_frame(&frame, 1);
    send_bytes(peer_fd, frame.data, 2);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 0 && connection.input.length == 2,
          "partial header stays in input buffer");
    send_bytes(peer_fd, frame.data + 2, 3);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 0 && connection.input.length == 5,
          "complete header but partial body stays buffered");
    send_bytes(peer_fd, frame.data + 5, frame.length - 5);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && connection.input.length == 0,
          "frame is consumed only after all body bytes arrive");
    check_response_frames(&connection.output, 1);

    buffer_destroy(&frame);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after fragmented frame");
}

static void test_coalesced_frames_and_budget(void)
{
    Connection connection = {0};
    ByteBuffer frames = {0};
    int peer_fd;
    size_t frames_processed = 0;

    open_test_connection(&connection, &peer_fd);
    check(buffer_init(&frames, 4096) == BUFFER_OK, "allocate twenty ping frames");
    for (unsigned frame_number = 1; frame_number <= 20; ++frame_number) {
        append_ping_frame(&frames, frame_number);
    }
    send_bytes(peer_fd, frames.data, frames.length);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 16 && connection.pending_frame,
          "first turn handles sixteen frames and marks buffered work pending");
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 4 && !connection.pending_frame &&
              connection.input.length == 0,
          "second turn handles remaining frames without another send");
    check_response_frames(&connection.output, 20);
    check(connection_flush(&connection), "flush queued coalesced response frames");

    buffer_destroy(&frames);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after coalesced frames");
}

static void test_frame_larger_than_one_read(void)
{
    Connection connection = {0};
    ByteBuffer frame = {0};
    char body[5000];
    int peer_fd;
    size_t frames_processed = 0;

    open_test_connection(&connection, &peer_fd);
    int prefix_length = snprintf(body, sizeof(body),
                                 "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                                 "\"request_id\":\"1\",\"data\":{}}");
    check(prefix_length > 0 && (size_t)prefix_length < sizeof(body),
          "build valid large ping body");
    memset(body + prefix_length, ' ', sizeof(body) - (size_t)prefix_length);
    check(buffer_init(&frame, sizeof(body) + 4) == BUFFER_OK,
          "allocate frame larger than receive chunk");
    check(frame_encode(&frame, body, sizeof(body)) == FRAME_OK,
          "encode large frame");
    send_bytes(peer_fd, frame.data, frame.length);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 0 && connection.input.length == 4096,
          "first recv keeps incomplete large frame");
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && connection.input.length == 0,
          "second recv finishes and consumes large frame");
    check_response_frames(&connection.output, 1);

    buffer_destroy(&frame);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after large frame");
}

static void test_invalid_length_and_eof(void)
{
    Connection connection = {0};
    int peer_fd;
    size_t frames_processed = 0;
    const unsigned char invalid_header[] = {0, 1, 0, 1};

    open_test_connection(&connection, &peer_fd);
    send_bytes(peer_fd, invalid_header, sizeof(invalid_header));
    check(connection_receive(&connection, &frames_processed) ==
              CONNECTION_RECEIVE_INVALID_FRAME,
          "oversized frame length closes only this connection");
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close invalid-frame peer");

    connection = (Connection){0};
    open_test_connection(&connection, &peer_fd);
    const unsigned char partial_header[] = {0, 0};
    send_bytes(peer_fd, partial_header, sizeof(partial_header));
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              connection.input.length == 2,
          "partial frame remains until peer closes");
    check(shutdown(peer_fd, SHUT_WR) == 0, "peer ends its write side");
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_EOF,
          "recv zero reports EOF rather than an empty frame");
    check(connection.peer_eof, "connection remembers that peer stopped sending");
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close EOF peer");
}

static void test_half_close_keeps_response_available(void)
{
    Connection connection = {0};
    int peer_fd;
    size_t frames_processed = 0;
    ByteBuffer frame = {0};
    ByteBuffer expected = {0};

    open_test_connection(&connection, &peer_fd);
    make_ping_frame(&frame, 1);
    send_bytes(peer_fd, frame.data, frame.length);
    check(shutdown(peer_fd, SHUT_WR) == 0, "peer half-closes after sending one frame");
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && connection.output.length > 0,
          "server queues the complete response before observing EOF");
    check_response_frames(&connection.output, 1);
    check(buffer_init(&expected, connection.output.length) == BUFFER_OK &&
              buffer_append(&expected, connection.output.data,
                            connection.output.length) == BUFFER_OK,
          "retain expected response before flush");
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_EOF &&
              connection.peer_eof && connection.output.length == expected.length,
          "EOF preserves queued response for the peer's read side");
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_EOF &&
              frames_processed == 0,
          "a closed read side is not read again");
    check(connection_flush(&connection) && connection.output.length == 0,
          "server can flush response after receiving EOF");
    unsigned char received[256] = {0};
    check(expected.length <= sizeof(received) &&
              recv(peer_fd, received, sizeof(received), 0) == (ssize_t)expected.length &&
              memcmp(received, expected.data, expected.length) == 0,
          "half-closed peer receives the complete response");

    buffer_destroy(&frame);
    buffer_destroy(&expected);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after half-close test");
}

static void test_separate_client_buffers(void)
{
    Connection first = {0};
    Connection second = {0};
    int first_peer;
    int second_peer;
    size_t frames_processed = 0;
    ByteBuffer first_frame = {0};
    ByteBuffer second_frame = {0};

    open_test_connection(&first, &first_peer);
    open_test_connection(&second, &second_peer);
    make_ping_frame(&first_frame, 1);
    make_ping_frame(&second_frame, 1);
    send_bytes(first_peer, first_frame.data, 3);
    send_bytes(second_peer, second_frame.data, second_frame.length);
    check(connection_receive(&first, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 0 && first.input.length == 3,
          "first client retains its incomplete header");
    check(connection_receive(&second, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && second.input.length == 0,
          "second client completes its frame independently");
    send_bytes(first_peer, first_frame.data + 3, first_frame.length - 3);
    check(connection_receive(&first, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && first.input.length == 0,
          "first client finishes without using second client's bytes");
    check_response_frames(&first.output, 1);
    check_response_frames(&second.output, 1);

    buffer_destroy(&first_frame);
    buffer_destroy(&second_frame);
    connection_destroy(&first);
    connection_destroy(&second);
    check(close(first_peer) == 0 && close(second_peer) == 0,
          "close both test peers");
}

static void test_partial_write_resume(void)
{
    Connection connection = {0};
    int peer_fd;
    open_test_connection(&connection, &peer_fd);

    int small_send_buffer = 1024;
    check(setsockopt(connection.fd, SOL_SOCKET, SO_SNDBUF,
                     &small_send_buffer, sizeof(small_send_buffer)) == 0,
          "limit server socket send buffer");
    unsigned char body[32768] = {0};
    ByteBuffer expected = {0};
    check(buffer_init(&expected, sizeof(body) + 4) == BUFFER_OK,
          "allocate expected large echo frame");
    check(frame_encode(&expected, body, sizeof(body)) == FRAME_OK &&
              frame_encode(&connection.output, body, sizeof(body)) == FRAME_OK,
          "queue large echo frame");

    check(connection_flush(&connection), "first nonblocking send succeeds");
    check(connection.output_offset > 0 &&
              connection.output_offset < connection.output.length &&
              connection.output_last_progress_ms > 0,
          "small kernel send buffer forces a partial write");

    unsigned char observed[sizeof(body) + 4];
    size_t received_total = 0;
    for (int attempt = 0; attempt < 10000 && received_total < sizeof(observed); ++attempt) {
        ssize_t received = recv(peer_fd, observed + received_total,
                                sizeof(observed) - received_total, 0);
        if (received > 0) {
            received_total += (size_t)received;
        } else {
            check(received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
                  "peer waits for remaining echo bytes");
        }
        check(connection_flush(&connection), "resume nonblocking send from saved offset");
    }
    check(received_total == sizeof(observed) &&
              memcmp(observed, expected.data, sizeof(observed)) == 0,
          "all bytes arrive in order after partial writes");
    check(connection.output.length == 0 && connection.output_offset == 0,
          "output queue is empty after complete flush");

    buffer_destroy(&expected);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after partial write");
}

static void test_echo_output_limit(void)
{
    Connection connection = {0};
    int peer_fd;
    size_t frames_processed = 0;
    unsigned char maximum_body[65536] = {0};
    ByteBuffer new_frame = {0};

    open_test_connection(&connection, &peer_fd);
    make_ping_frame(&new_frame, 1);
    check(frame_encode(&connection.output, maximum_body, sizeof(maximum_body)) == FRAME_OK &&
              frame_encode(&connection.output, maximum_body, sizeof(maximum_body)) == FRAME_OK,
          "fill fixed-capacity diagnostic echo queue");
    send_bytes(peer_fd, new_frame.data, new_frame.length);
    check(connection_receive(&connection, &frames_processed) ==
              CONNECTION_RECEIVE_OUTPUT_FULL && frames_processed == 0,
          "full output closes client instead of dropping a response silently");

    buffer_destroy(&new_frame);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after output limit");
}

static void test_output_no_progress_deadline(void)
{
    Connection connection = {0};
    int peer_fd;
    size_t frames_processed = 0;
    ByteBuffer frame = {0};

    open_test_connection(&connection, &peer_fd);
    make_ping_frame(&frame, 1);
    send_bytes(peer_fd, frame.data, frame.length);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && connection.output_last_progress_ms > 0,
          "queueing the first response starts the output progress clock");
    int64_t now_ms = test_monotonic_milliseconds();
    connection.output_last_progress_ms = now_ms -
                                         CHAT_OUTPUT_NO_PROGRESS_TIMEOUT_MS + 1000;
    check(!connection_output_timed_out(&connection),
          "pending output remains active before the fifteen-second deadline");
    connection.output_last_progress_ms = now_ms -
                                         CHAT_OUTPUT_NO_PROGRESS_TIMEOUT_MS - 1000;
    check(connection_output_timed_out(&connection),
          "pending output is timed out when no byte progresses for fifteen seconds");
    check(connection_flush(&connection) && connection.output.length == 0 &&
              connection.output_last_progress_ms == 0 &&
              !connection_output_timed_out(&connection),
          "a complete flush clears the output deadline");

    buffer_destroy(&frame);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after output deadline test");
}

static void test_router_error_responses_and_close(void)
{
    const char *bodies[] = {
        "not-json",
        "{\"bad\":true}",
        "{\"v\":1,\"kind\":\"request\",\"type\":\"unknown.action\","
        "\"request_id\":\"1\",\"data\":{}}",
        "{\"v\":2,\"kind\":\"request\",\"type\":\"system.ping\","
        "\"request_id\":\"1\",\"data\":{}}",
        "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
        "\"request_id\":\"1\",\"data\":{\"extra\":true}}"
    };
    const char *codes[] = {
        "INVALID_JSON", "INVALID_REQUEST", "UNKNOWN_TYPE",
        "UNSUPPORTED_VERSION", "VALIDATION_ERROR"
    };
    const bool closes[] = {true, true, false, true, false};

    for (size_t index = 0; index < sizeof(bodies) / sizeof(bodies[0]); ++index) {
        Connection connection = {0};
        ByteBuffer frame = {0};
        int peer_fd;
        size_t frames_processed = 0;
        open_test_connection(&connection, &peer_fd);
        check(buffer_init(&frame, 256) == BUFFER_OK &&
                  frame_encode(&frame, bodies[index], strlen(bodies[index])) == FRAME_OK,
              "encode request that router must reject");
        send_bytes(peer_fd, frame.data, frame.length);
        check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
                  frames_processed == 1 && connection.output.length > 0 &&
                  connection.close_after_flush == closes[index],
              "router queues error and chooses whether to close after flush");
        check_error_frame(&connection.output, 0,
                          index < 2 ? "event" : "response",
                          index < 2 ? "protocol.error" :
                          index == 2 ? "unknown.action" : "system.ping",
                          index < 2 ? NULL : "1", codes[index]);
        if (closes[index]) {
            check(connection.close_deadline_ms > 0 &&
                      !connection_close_timed_out(&connection),
                  "fatal error starts a bounded close-after-flush deadline");
            connection.close_deadline_ms = test_monotonic_milliseconds() - 1;
            check(connection_close_timed_out(&connection),
                  "fatal error reaches two-second close deadline");
        }
        buffer_destroy(&frame);
        connection_destroy(&connection);
        check(close(peer_fd) == 0, "close rejected request peer");
    }
}

static void test_valid_then_invalid_keeps_queued_response(void)
{
    Connection connection = {0};
    ByteBuffer frames = {0};
    int peer_fd;
    size_t frames_processed = 0;
    open_test_connection(&connection, &peer_fd);
    check(buffer_init(&frames, 512) == BUFFER_OK, "allocate coalesced valid/error requests");
    append_ping_frame(&frames, 1);
    const char invalid[] = "not-json";
    check(frame_encode(&frames, invalid, strlen(invalid)) == FRAME_OK,
          "append malformed JSON after valid ping");
    append_ping_frame(&frames, 2);
    send_bytes(peer_fd, frames.data, frames.length);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 2 && connection.close_after_flush &&
              !connection.pending_frame && connection.input.length > 0,
          "fatal error stops reading after two frames and leaves later request unprocessed");
    check_error_frame(&connection.output, 1, "event", "protocol.error", NULL,
                      "INVALID_JSON");
    ByteBuffer copy = {0};
    check(buffer_init(&copy, connection.output.length) == BUFFER_OK &&
              buffer_append(&copy, connection.output.data, connection.output.length) == BUFFER_OK,
          "copy both queued responses");
    FrameView view = {0};
    check(frame_peek(&copy, &view) == FRAME_OK, "valid ping response remains first");
    check(frame_consume(&copy) == FRAME_OK && frame_peek(&copy, &view) == FRAME_OK,
          "protocol.error follows earlier response");
    check(frame_consume(&copy) == FRAME_OK && copy.length == 0,
          "later ping has no response after fatal error");
    unsigned char expected[512];
    size_t expected_length = connection.output.length;
    check(expected_length <= sizeof(expected), "ordered responses fit test buffer");
    memcpy(expected, connection.output.data, expected_length);
    check(connection_flush(&connection) && connection.output.length == 0,
          "flush both responses before closing connection");
    unsigned char received[512];
    size_t received_length = 0;
    for (int attempt = 0; attempt < 100 && received_length < expected_length; ++attempt) {
        ssize_t count = recv(peer_fd, received + received_length,
                             expected_length - received_length, 0);
        check(count > 0 || (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)),
              "peer reads queued success and error frames");
        if (count > 0) {
            received_length += (size_t)count;
        }
    }
    check(received_length == expected_length &&
              memcmp(received, expected, expected_length) == 0,
          "peer receives both complete frames in order");
    buffer_destroy(&copy);
    buffer_destroy(&frames);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after ordered error flush");
}

static void test_connection_close_flushes_and_stops_dispatch(void)
{
    Connection connection = {0};
    ByteBuffer frames = {0};
    int peer_fd;
    size_t frames_processed = 0;
    open_test_connection(&connection, &peer_fd);
    check(buffer_init(&frames, 512) == BUFFER_OK, "allocate ping/close/ping sequence");
    append_ping_frame(&frames, 1);
    const char close_body[] = "{\"v\":1,\"kind\":\"request\","
                              "\"type\":\"connection.close\",\"request_id\":\"2\","
                              "\"data\":{}}";
    check(frame_encode(&frames, close_body, strlen(close_body)) == FRAME_OK,
          "encode connection.close after ping");
    append_ping_frame(&frames, 3);
    send_bytes(peer_fd, frames.data, frames.length);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 2 && connection.close_after_flush &&
              connection.close_deadline_ms > test_monotonic_milliseconds() &&
              connection.input.length > 0 && !connection.pending_frame,
          "close queues final response and stops before later ping");

    ByteBuffer copy = {0};
    check(buffer_init(&copy, connection.output.length) == BUFFER_OK &&
              buffer_append(&copy, connection.output.data, connection.output.length) == BUFFER_OK,
          "copy responses queued before graceful close");
    check(frame_consume(&copy) == FRAME_OK, "first frame is prior ping response");
    FrameView view = {0};
    check(frame_peek(&copy, &view) == FRAME_OK, "second frame is close response");
    char json[256];
    check(view.length < sizeof(json), "close response fits test buffer");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    cJSON *response = cJSON_Parse(json);
    check(response != NULL, "close response contains valid JSON");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(response, "type");
    const cJSON *request_id = cJSON_GetObjectItemCaseSensitive(response, "request_id");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
    check(cJSON_IsString(type) && strcmp(type->valuestring, "connection.close") == 0 &&
              cJSON_IsString(request_id) && strcmp(request_id->valuestring, "2") == 0 &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")) &&
              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "closing")),
          "close response confirms graceful shutdown with matching request ID");
    cJSON_Delete(response);
    check(frame_consume(&copy) == FRAME_OK && copy.length == 0,
          "request after close receives no response");
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 0 && connection.input.length > 0,
          "closing connection does not read or dispatch another request");
    check(connection_flush(&connection) && connection.output.length == 0 &&
              connection.close_after_flush,
          "final response can be flushed before main closes the socket");

    buffer_destroy(&copy);
    buffer_destroy(&frames);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after graceful-close test");

    connection = (Connection){0};
    open_test_connection(&connection, &peer_fd);
    const char invalid_body[] = "{\"v\":1,\"kind\":\"request\","
                                "\"type\":\"connection.close\",\"request_id\":\"1\","
                                "\"data\":{\"extra\":true}}";
    check(buffer_init(&frames, 256) == BUFFER_OK &&
              frame_encode(&frames, invalid_body, strlen(invalid_body)) == FRAME_OK,
          "encode connection.close with invalid data");
    send_bytes(peer_fd, frames.data, frames.length);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && !connection.close_after_flush,
          "invalid close data does not terminate connection");
    check_error_frame(&connection.output, 0, "response", "connection.close", "1",
                      "VALIDATION_ERROR");
    buffer_destroy(&frames);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after invalid-close test");
}

static void test_ready_precedes_first_response(void)
{
    Connection connection = {0};
    ByteBuffer request_frame = {0};
    int peer_fd;
    size_t frames_processed = 0;
    open_test_connection(&connection, &peer_fd);
    check(connection_queue_ready(&connection) && connection.ready_queued &&
              connection.output_last_progress_ms > 0,
          "accept queues ready and starts output timer");
    size_t ready_length = connection.output.length;
    check(!connection_queue_ready(&connection) && connection.output.length == ready_length,
          "ready cannot be queued twice for the same connection");
    make_ping_frame(&request_frame, 1);
    send_bytes(peer_fd, request_frame.data, request_frame.length);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "ping can arrive while ready is still in output queue");

    ByteBuffer queued = {0};
    check(buffer_init(&queued, connection.output.length) == BUFFER_OK &&
              buffer_append(&queued, connection.output.data, connection.output.length) == BUFFER_OK,
          "copy ready and ping response for order inspection");
    FrameView view = {0};
    check(frame_peek(&queued, &view) == FRAME_OK &&
              protocol_validate_ready_event(view.body, view.length) == PROTOCOL_OK,
          "first output frame is a valid connection.ready event");
    check(frame_consume(&queued) == FRAME_OK,
          "consume ready before inspecting ping response");
    check_response_frames(&queued, 1);
    check(connection_flush(&connection) && connection.output.length == 0,
          "ready and response flush through the same nonblocking queue");

    buffer_destroy(&queued);
    buffer_destroy(&request_frame);
    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after ready-order test");
}

static void send_control_pong(int fd, const char *nonce)
{
    char body[128];
    int length = snprintf(body, sizeof(body),
                          "{\"v\":1,\"kind\":\"control\",\"type\":\"heartbeat.pong\","
                          "\"data\":{\"nonce\":\"%s\"}}", nonce);
    check(length > 0 && (size_t)length < sizeof(body), "format pong body");
    ByteBuffer frame = {0};
    check(buffer_init(&frame, 256) == BUFFER_OK, "init pong frame buffer");
    check(frame_encode(&frame, body, (size_t)length) == FRAME_OK, "encode pong frame");
    send_bytes(fd, frame.data, frame.length);
    buffer_destroy(&frame);
}

static void test_heartbeat_pong_clears_outstanding(void)
{
    Connection connection = {0};
    int peer_fd = -1;
    open_test_connection(&connection, &peer_fd);

    const char test_nonce[] = "0123456789abcdef0123456789abcdef";
    check(connection_queue_heartbeat_ping(&connection, test_nonce, 1000000000LL),
          "queue heartbeat.ping event");
    check(connection.heartbeat_outstanding &&
              strcmp(connection.heartbeat_nonce, test_nonce) == 0,
          "connection remembers outstanding ping nonce");

    /* Không cho queue ping lần 2 khi chưa có pong. */
    check(!connection_queue_heartbeat_ping(&connection, test_nonce, 1000000000LL),
          "cannot queue second ping while first is outstanding");

    /* Pong hợp lệ từ peer xóa trạng thái outstanding và không sinh response. */
    send_control_pong(peer_fd, test_nonce);
    size_t frames_processed = 0;
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "receive and process heartbeat.pong");
    check(!connection.heartbeat_outstanding && connection.heartbeat_nonce[0] == '\0' &&
              connection.invalid_pong_count == 0,
          "valid pong clears outstanding ping and resets invalid pong count");

    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after pong test");
}

static void test_heartbeat_invalid_pongs_and_timeout(void)
{
    Connection connection = {0};
    int peer_fd = -1;
    open_test_connection(&connection, &peer_fd);

    const char valid_nonce[] = "0123456789abcdef0123456789abcdef";
    const char wrong_nonce[] = "ffffffffffffffffffffffffffffffff";

    check(connection_queue_heartbeat_ping(&connection, valid_nonce, 1000000000LL),
          "queue ping with valid nonce");

    /* Gửi pong sai nonce lần 1. */
    send_control_pong(peer_fd, wrong_nonce);
    size_t frames_processed = 0;
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && connection.invalid_pong_count == 1 &&
              connection.heartbeat_outstanding && !connection.close_after_flush,
          "mismatched pong increments invalid count without closing immediately");

    /* Gửi pong sai nonce lần 2. */
    send_control_pong(peer_fd, wrong_nonce);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && connection.invalid_pong_count == 2 &&
              !connection.close_after_flush,
          "second mismatched pong still keeps connection alive");

    /* Gửi pong sai nonce lần 3: đóng sau flush. */
    send_control_pong(peer_fd, wrong_nonce);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1 && connection.invalid_pong_count == 3 &&
              connection.close_after_flush && connection.close_deadline_ms > 0,
          "third mismatched pong triggers close_after_flush");

    /* Timeout check. */
    connection.heartbeat_deadline_ms = 1; /* Thời điểm trong quá khứ */
    check(connection_heartbeat_timed_out(&connection),
          "connection_heartbeat_timed_out reports true after deadline");

    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after invalid pong test");
}

static void test_idle_heartbeat_detection(void)
{
    Connection connection = {0};
    int peer_fd = -1;
    open_test_connection(&connection, &peer_fd);

    check(!connection_needs_heartbeat_ping(&connection),
          "new connection does not need ping immediately");

    /* Mô phỏng kết nối im lặng quá 30 giây */
    connection.last_activity_ms -= (CHAT_HEARTBEAT_IDLE_TIMEOUT_MS + 1000);
    check(connection_needs_heartbeat_ping(&connection),
          "connection idle > 30s needs heartbeat ping");

    /* Khi đã queue ping (outstanding = true), không trigger thêm ping mới */
    const char nonce[] = "0123456789abcdef0123456789abcdef";
    check(connection_queue_heartbeat_ping(&connection, nonce, 1000000000LL),
          "queue heartbeat ping");
    check(!connection_needs_heartbeat_ping(&connection),
          "cannot trigger ping while another ping is outstanding");

    /* Nhận pong hợp lệ: xóa outstanding và làm mới last_activity_ms */
    send_control_pong(peer_fd, nonce);
    size_t frames_processed = 0;
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "receive and process valid pong");
    check(!connection_needs_heartbeat_ping(&connection),
          "activity refreshed after valid pong");

    /* Khi đang ở trạng thái close_after_flush thì không phát ping */
    connection.last_activity_ms -= (CHAT_HEARTBEAT_IDLE_TIMEOUT_MS + 1000);
    connection.close_after_flush = true;
    check(!connection_needs_heartbeat_ping(&connection),
          "no ping when connection is closing after flush");

    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after idle test");
}

static void test_high_water_and_request_cache(void)
{
    Connection connection = {0};
    int peer_fd = -1;
    open_test_connection(&connection, &peer_fd);

    /* 1. Gửi request 1 (system.ping) */
    ByteBuffer frame = {0};
    make_ping_frame(&frame, 1);
    send_bytes(peer_fd, frame.data, frame.length);
    buffer_destroy(&frame);

    size_t frames_processed = 0;
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "receive ping request 1");
    check(connection.high_water_request_id == 1, "high water mark updated to 1");

    FrameView view = {0};
    check(frame_peek(&connection.output, &view) == FRAME_OK, "response 1 is queued");
    char json1[256];
    memcpy(json1, view.body, view.length);
    json1[view.length] = '\0';
    cJSON *resp1 = cJSON_Parse(json1);
    check(resp1 != NULL, "response 1 is valid JSON");
    const cJSON *data1 = cJSON_GetObjectItemCaseSensitive(resp1, "data");
    check(data1 != NULL, "response 1 has data");
    double server_time1 = cJSON_GetObjectItemCaseSensitive(data1, "server_time")->valuedouble;
    cJSON_Delete(resp1);

    /* Xả output */
    buffer_consume(&connection.output, connection.output.length);

    /* 2. Replay request 1 (cùng type + data): phải trả lại response cũ (cùng server_time) */
    make_ping_frame(&frame, 1);
    send_bytes(peer_fd, frame.data, frame.length);
    buffer_destroy(&frame);

    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "receive replayed ping request 1");
    check(frame_peek(&connection.output, &view) == FRAME_OK, "replayed response 1 queued");
    char json1_replay[256];
    memcpy(json1_replay, view.body, view.length);
    json1_replay[view.length] = '\0';
    cJSON *resp1_replay = cJSON_Parse(json1_replay);
    check(resp1_replay != NULL, "replayed response 1 is valid JSON");
    const cJSON *data1_replay = cJSON_GetObjectItemCaseSensitive(resp1_replay, "data");
    double server_time1_replay = cJSON_GetObjectItemCaseSensitive(data1_replay, "server_time")->valuedouble;
    check(server_time1 == server_time1_replay, "replayed response matches cached server_time exactly");
    cJSON_Delete(resp1_replay);
    buffer_consume(&connection.output, connection.output.length);

    /* 3. Reused ID: request 1 nhưng đổi data hoặc type -> REQUEST_ID_REUSED, không đóng kết nối */
    const char reused_body[] = "{\"v\":1,\"kind\":\"request\",\"type\":\"system.ping\","
                               "\"request_id\":\"1\",\"data\":{\"unexpected\":1}}";
    check(buffer_init(&frame, 256) == BUFFER_OK, "allocate frame");
    check(frame_encode(&frame, reused_body, strlen(reused_body)) == FRAME_OK, "encode reused frame");
    send_bytes(peer_fd, frame.data, frame.length);
    buffer_destroy(&frame);

    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "receive reused request 1");
    check(!connection.close_after_flush, "reused request does not close connection");
    check(frame_peek(&connection.output, &view) == FRAME_OK, "error response queued");
    char json_reused[256];
    memcpy(json_reused, view.body, view.length);
    json_reused[view.length] = '\0';
    cJSON *resp_reused = cJSON_Parse(json_reused);
    check(resp_reused != NULL, "reused response is valid JSON");
    const cJSON *err_reused = cJSON_GetObjectItemCaseSensitive(resp_reused, "error");
    check(err_reused != NULL &&
              strcmp(cJSON_GetObjectItemCaseSensitive(err_reused, "code")->valuestring,
                     "REQUEST_ID_REUSED") == 0,
          "error code is REQUEST_ID_REUSED");
    cJSON_Delete(resp_reused);
    buffer_consume(&connection.output, connection.output.length);

    /* 4. Request 5: nhảy cóc request_id -> high_water thành 5 */
    make_ping_frame(&frame, 5);
    send_bytes(peer_fd, frame.data, frame.length);
    buffer_destroy(&frame);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "receive ping request 5");
    check(connection.high_water_request_id == 5, "high water mark jumped to 5");
    buffer_consume(&connection.output, connection.output.length);

    /* 5. Request 3: ID <= high_water (3 <= 5) và không có trong cache -> REQUEST_ID_EXPIRED */
    make_ping_frame(&frame, 3);
    send_bytes(peer_fd, frame.data, frame.length);
    buffer_destroy(&frame);
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "receive out-of-order ping request 3");
    check(!connection.close_after_flush, "expired request does not close connection");
    check(frame_peek(&connection.output, &view) == FRAME_OK, "error response queued");
    char json_expired[256];
    memcpy(json_expired, view.body, view.length);
    json_expired[view.length] = '\0';
    cJSON *resp_expired = cJSON_Parse(json_expired);
    check(resp_expired != NULL, "expired response is valid JSON");
    const cJSON *err_expired = cJSON_GetObjectItemCaseSensitive(resp_expired, "error");
    check(err_expired != NULL &&
              strcmp(cJSON_GetObjectItemCaseSensitive(err_expired, "code")->valuestring,
                     "REQUEST_ID_EXPIRED") == 0,
          "error code is REQUEST_ID_EXPIRED");
    cJSON_Delete(resp_expired);

    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after cache test");
}

static void test_request_cache_eviction(void)
{
    Connection connection = {0};
    int peer_fd = -1;
    open_test_connection(&connection, &peer_fd);

    /* Gửi 65 requests liên tiếp (1..65); cache tối đa 64 entries nên request 1 bị evict */
    for (unsigned i = 1; i <= 65; ++i) {
        ByteBuffer frame = {0};
        make_ping_frame(&frame, i);
        send_bytes(peer_fd, frame.data, frame.length);
        buffer_destroy(&frame);

        size_t frames_processed = 0;
        check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
                  frames_processed == 1,
              "process sequential ping frame");
        /* Xả output để tránh đầy output buffer */
        buffer_consume(&connection.output, connection.output.length);
    }
    check(connection.high_water_request_id == 65, "high water mark is 65");
    check(connection.request_cache.count == CHAT_REQUEST_CACHE_MAX_ENTRIES,
          "cache is at max 64 entries");

    /* Gửi lại request 1: đã bị evict khỏi cache FIFO -> trả về REQUEST_ID_EXPIRED */
    ByteBuffer frame = {0};
    make_ping_frame(&frame, 1);
    send_bytes(peer_fd, frame.data, frame.length);
    buffer_destroy(&frame);

    size_t frames_processed = 0;
    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "process evicted request 1");
    FrameView view = {0};
    check(frame_peek(&connection.output, &view) == FRAME_OK, "error response queued");
    char json[256];
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    cJSON *resp = cJSON_Parse(json);
    check(resp != NULL, "parsed response JSON");
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    check(err != NULL &&
              strcmp(cJSON_GetObjectItemCaseSensitive(err, "code")->valuestring,
                     "REQUEST_ID_EXPIRED") == 0,
          "evicted request yields REQUEST_ID_EXPIRED");
    cJSON_Delete(resp);
    buffer_consume(&connection.output, connection.output.length);

    /* Gửi lại request 65: vẫn còn trong cache -> replay thành công */
    make_ping_frame(&frame, 65);
    send_bytes(peer_fd, frame.data, frame.length);
    buffer_destroy(&frame);

    check(connection_receive(&connection, &frames_processed) == CONNECTION_RECEIVE_OK &&
              frames_processed == 1,
          "process replayed request 65");
    check(frame_peek(&connection.output, &view) == FRAME_OK, "response queued");
    memcpy(json, view.body, view.length);
    json[view.length] = '\0';
    resp = cJSON_Parse(json);
    check(resp != NULL, "parsed response JSON");
    check(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "ok")),
          "replayed request 65 is ok: true");
    cJSON_Delete(resp);

    connection_destroy(&connection);
    check(close(peer_fd) == 0, "close peer after eviction test");
}

int main(void)
{
    test_fragmented_header_and_body();
    test_coalesced_frames_and_budget();
    test_frame_larger_than_one_read();
    test_invalid_length_and_eof();
    test_half_close_keeps_response_available();
    test_separate_client_buffers();
    test_partial_write_resume();
    test_echo_output_limit();
    test_output_no_progress_deadline();
    test_router_error_responses_and_close();
    test_valid_then_invalid_keeps_queued_response();
    test_connection_close_flushes_and_stops_dispatch();
    test_ready_precedes_first_response();
    test_heartbeat_pong_clears_outstanding();
    test_heartbeat_invalid_pongs_and_timeout();
    test_idle_heartbeat_detection();
    test_high_water_and_request_cache();
    test_request_cache_eviction();
    puts("18 connection tests passed.");
    return EXIT_SUCCESS;
}
