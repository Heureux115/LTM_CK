/* Kiểm tra codec khi frame bị chia nhỏ, ghép liền và gặp header sai. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/buffer.h"
#include "shared/frame.h"
#include "shared/limits.h"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_big_endian_encode(void)
{
    ByteBuffer output = {0};
    FrameView view = {0};
    const unsigned char expected[] = {0, 0, 0, 2, '{', '}'};

    check(buffer_init(&output, sizeof(expected)) == BUFFER_OK, "initialize encoded output");
    check(frame_encode(&output, "{}", 2) == FRAME_OK, "encode two-byte body");
    check(output.length == sizeof(expected), "encoded frame includes four-byte header");
    check(memcmp(output.data, expected, sizeof(expected)) == 0,
          "header stores body length in big-endian order");
    check(frame_peek(&output, &view) == FRAME_OK, "decode encoded frame");
    check(view.length == 2 && memcmp(view.body, "{}", 2) == 0,
          "decoded view contains body without header");
    check(frame_consume(&output) == FRAME_OK && output.length == 0,
          "consume exactly one encoded frame");
    buffer_destroy(&output);
}

static void test_fragmented_header_and_body(void)
{
    ByteBuffer input = {0};
    FrameView view = {0};
    const unsigned char frame[] = {0, 0, 0, 3, 'a', 'b', 'c'};

    check(buffer_init(&input, sizeof(frame)) == BUFFER_OK, "initialize fragmented input");
    for (size_t byte_count = 1; byte_count <= sizeof(frame); ++byte_count) {
        check(buffer_append(&input, frame + byte_count - 1, 1) == BUFFER_OK,
              "append next received byte");
        FrameResult result = frame_peek(&input, &view);
        if (byte_count < sizeof(frame)) {
            check(result == FRAME_NEED_MORE, "wait until header and body are complete");
            check(view.body == NULL && view.length == 0, "incomplete frame has no view");
            check(input.length == byte_count, "peek preserves incomplete input");
        } else {
            check(result == FRAME_OK, "complete frame becomes ready");
            check(view.length == 3 && memcmp(view.body, "abc", 3) == 0,
                  "reassembled body matches original bytes");
        }
    }
    buffer_destroy(&input);
}

static void test_coalesced_frames(void)
{
    ByteBuffer input = {0};
    FrameView view = {0};

    check(buffer_init(&input, 60) == BUFFER_OK, "initialize multi-frame input");
    for (size_t frame_number = 0; frame_number < 10; ++frame_number) {
        unsigned char body = (unsigned char)('0' + frame_number);
        check(frame_encode(&input, &body, 1) == FRAME_OK, "append next complete frame");
    }
    for (size_t frame_number = 0; frame_number < 10; ++frame_number) {
        unsigned char expected = (unsigned char)('0' + frame_number);
        check(frame_peek(&input, &view) == FRAME_OK, "next coalesced frame is ready");
        check(view.length == 1 && view.body[0] == expected, "frame order is preserved");
        check(frame_consume(&input) == FRAME_OK, "consume one coalesced frame");
    }
    check(input.length == 0 && frame_peek(&input, &view) == FRAME_NEED_MORE,
          "all coalesced frames consumed");
    buffer_destroy(&input);
}

static void test_complete_then_partial_next(void)
{
    ByteBuffer input = {0};
    FrameView view = {0};
    const unsigned char first[] = {0, 0, 0, 2, '{', '}'};
    const unsigned char second[] = {0, 0, 0, 3, 'x', 'y', 'z'};

    check(buffer_init(&input, sizeof(first) + sizeof(second)) == BUFFER_OK,
          "initialize mixed input");
    check(buffer_append(&input, first, sizeof(first)) == BUFFER_OK, "receive first frame");
    check(buffer_append(&input, second, 5) == BUFFER_OK, "receive partial second frame");
    check(frame_peek(&input, &view) == FRAME_OK && view.length == 2,
          "first frame ready before second is complete");
    check(frame_consume(&input) == FRAME_OK, "consume first frame only");
    check(input.length == 5 && frame_peek(&input, &view) == FRAME_NEED_MORE,
          "preserve incomplete second frame");
    check(frame_consume(&input) == FRAME_NEED_MORE && input.length == 5,
          "cannot consume incomplete second frame");
    check(buffer_append(&input, second + 5, sizeof(second) - 5) == BUFFER_OK,
          "receive remaining second-frame bytes");
    check(frame_peek(&input, &view) == FRAME_OK, "second frame becomes ready");
    check(view.length == 3 && memcmp(view.body, "xyz", 3) == 0,
          "second frame body is intact");
    check(frame_consume(&input) == FRAME_OK && input.length == 0,
          "consume second frame after completion");
    buffer_destroy(&input);
}

static void test_invalid_length_rejected_at_header(void)
{
    ByteBuffer input = {0};
    FrameView view = {0};
    const unsigned char empty_length[] = {0, 0, 0, 0};
    const unsigned char too_large[] = {0, 1, 0, 1};
    const unsigned char maximum_uint32[] = {0xff, 0xff, 0xff, 0xff};
    const unsigned char *invalid_headers[] = {empty_length, too_large, maximum_uint32};

    check(buffer_init(&input, 4) == BUFFER_OK, "initialize invalid-length input");
    for (size_t header_index = 0; header_index < 3; ++header_index) {
        check(buffer_append(&input, invalid_headers[header_index], 4) == BUFFER_OK,
              "receive complete invalid header");
        check(frame_peek(&input, &view) == FRAME_INVALID_LENGTH,
              "reject zero, oversized, or uint32-max length without body");
        check(view.body == NULL && view.length == 0,
              "invalid header never exposes body view");
        check(frame_consume(&input) == FRAME_INVALID_LENGTH && input.length == 4,
              "invalid frame remains for caller to close connection");
        check(buffer_consume(&input, 4) == BUFFER_OK, "reset test input");
    }
    buffer_destroy(&input);
}

static void test_maximum_body_and_binary_bytes(void)
{
    ByteBuffer input = {0};
    FrameView view = {0};
    unsigned char body[CHAT_MAX_FRAME_BODY_BYTES];
    memset(body, 0xa5, sizeof(body));
    body[0] = 0;
    body[sizeof(body) - 1] = 0xff;

    check(buffer_init(&input, CHAT_FRAME_HEADER_BYTES + sizeof(body)) == BUFFER_OK,
          "initialize maximum-sized frame storage");
    check(frame_encode(&input, body, sizeof(body)) == FRAME_OK,
          "encode maximum permitted body");
    check(input.data[0] == 0 && input.data[1] == 1 &&
          input.data[2] == 0 && input.data[3] == 0,
          "maximum length header is 00 01 00 00");
    check(frame_peek(&input, &view) == FRAME_OK && view.length == sizeof(body),
          "decode maximum-sized body");
    check(memcmp(view.body, body, sizeof(body)) == 0,
          "frame codec preserves binary bytes including NUL");
    check(frame_consume(&input) == FRAME_OK && input.length == 0,
          "consume maximum-sized frame");
    buffer_destroy(&input);
}

static void test_encode_rejects_invalid_or_full(void)
{
    ByteBuffer output = {0};

    check(buffer_init(&output, 6) == BUFFER_OK, "initialize bounded output");
    check(frame_encode(&output, "{}", 2) == FRAME_OK, "fill output exactly");
    check(frame_encode(&output, "x", 1) == FRAME_BUFFER_FULL,
          "reject second frame when output is full");
    check(frame_encode(&output, "x", SIZE_MAX) == FRAME_INVALID_LENGTH,
          "reject enormous body before length conversion");
    check(frame_encode(&output, "x", CHAT_MAX_FRAME_BODY_BYTES + 1u) == FRAME_INVALID_LENGTH,
          "reject first body length above the protocol limit");
    check(frame_encode(&output, "", 0) == FRAME_INVALID_LENGTH,
          "reject empty body");
    check(frame_encode(&output, NULL, 1) == FRAME_INVALID_ARGUMENT,
          "reject NULL nonempty body");
    check(output.length == 6 && memcmp(output.data, "\0\0\0\2{}", 6) == 0,
          "invalid encodes preserve existing frame");
    buffer_destroy(&output);

    check(buffer_init(&output, 4) == BUFFER_OK, "initialize header-only output");
    check(frame_encode(&output, "x", 1) == FRAME_BUFFER_FULL && output.length == 0,
          "header alone is insufficient for a frame");
    buffer_destroy(&output);
}

static void test_invalid_arguments(void)
{
    ByteBuffer input = {0};
    FrameView view = {0};

    check(frame_encode(NULL, "x", 1) == FRAME_INVALID_ARGUMENT, "reject NULL output");
    check(frame_encode(&input, "x", 1) == FRAME_INVALID_ARGUMENT,
          "reject uninitialized output");
    check(frame_peek(NULL, &view) == FRAME_INVALID_ARGUMENT, "reject NULL input");
    check(frame_peek(&input, &view) == FRAME_INVALID_ARGUMENT,
          "reject uninitialized input");
    check(frame_peek(&input, NULL) == FRAME_INVALID_ARGUMENT, "reject NULL view");
    check(frame_consume(NULL) == FRAME_INVALID_ARGUMENT, "reject NULL consume input");
    check(frame_consume(&input) == FRAME_INVALID_ARGUMENT,
          "reject uninitialized consume input");

    check(buffer_init(&input, 4) == BUFFER_OK, "initialize empty input");
    check(frame_peek(&input, &view) == FRAME_NEED_MORE,
          "empty initialized input needs more data");
    check(view.body == NULL && view.length == 0, "NEED_MORE clears view");
    buffer_destroy(&input);
}

int main(void)
{
    test_big_endian_encode();
    test_fragmented_header_and_body();
    test_coalesced_frames();
    test_complete_then_partial_next();
    test_invalid_length_rejected_at_header();
    test_maximum_body_and_binary_bytes();
    test_encode_rejects_invalid_or_full();
    test_invalid_arguments();
    puts("8 frame tests passed.");
    return EXIT_SUCCESS;
}
