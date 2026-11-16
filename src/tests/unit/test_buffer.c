/* Kiểm tra ByteBuffer độc lập socket: cấp phát, ghép byte, bỏ byte và giới hạn. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/buffer.h"
#include "shared/limits.h"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_init_and_destroy(void)
{
    ByteBuffer buffer = {0};

    check(buffer_init(&buffer, 8) == BUFFER_OK, "initialize buffer");
    check(buffer.data != NULL && buffer.length == 0 && buffer.capacity == 8,
          "initialized buffer owns empty storage");
    check(buffer_append(&buffer, "abc", 3) == BUFFER_OK, "append before reinit");
    unsigned char *original_data = buffer.data;
    check(buffer_init(&buffer, 16) == BUFFER_INVALID_ARGUMENT, "reject live reinit");
    check(buffer.data == original_data && buffer.length == 3 && buffer.capacity == 8,
          "reinit failure preserves ownership");
    check(memcmp(buffer.data, "abc", 3) == 0, "reinit failure preserves bytes");

    buffer_destroy(&buffer);
    check(buffer.data == NULL && buffer.length == 0 && buffer.capacity == 0,
          "destroy resets all fields");
    buffer_destroy(&buffer);
    check(buffer_init(&buffer, 4) == BUFFER_OK, "reinitialize after destroy");
    buffer_destroy(&buffer);
}

static void test_binary_chunks(void)
{
    ByteBuffer buffer = {0};
    const unsigned char first[] = {0x41, 0x00};
    unsigned char second[] = {0xff, 0x42};
    const unsigned char expected[] = {0x41, 0x00, 0xff, 0x42};

    check(buffer_init(&buffer, sizeof(expected)) == BUFFER_OK, "initialize binary buffer");
    check(buffer_append(&buffer, first, sizeof(first)) == BUFFER_OK, "append first chunk");
    check(buffer_append(&buffer, second, sizeof(second)) == BUFFER_OK, "append second chunk");
    second[0] = 0;
    check(buffer.length == sizeof(expected), "binary length includes NUL byte");
    check(memcmp(buffer.data, expected, sizeof(expected)) == 0,
          "append copies binary bytes in order");
    buffer_destroy(&buffer);
}

static void test_consume_and_reuse(void)
{
    ByteBuffer buffer = {0};

    check(buffer_init(&buffer, 5) == BUFFER_OK, "initialize consume buffer");
    check(buffer_append(&buffer, "HEL", 3) == BUFFER_OK, "append HEL");
    check(buffer_append(&buffer, "LO", 2) == BUFFER_OK, "append LO");
    check(buffer_consume(&buffer, 3) == BUFFER_OK, "consume first three bytes");
    check(buffer.length == 2 && memcmp(buffer.data, "LO", 2) == 0,
          "consume compacts remaining bytes");
    check(buffer_append(&buffer, "123", 3) == BUFFER_OK, "reuse consumed space");
    check(buffer.length == 5 && memcmp(buffer.data, "LO123", 5) == 0,
          "append after compaction preserves order");
    check(buffer_consume(&buffer, 0) == BUFFER_OK && buffer.length == 5,
          "consume zero is a no-op");
    check(buffer_consume(&buffer, 6) == BUFFER_INVALID_ARGUMENT,
          "reject consume beyond available bytes");
    check(buffer.length == 5 && memcmp(buffer.data, "LO123", 5) == 0,
          "invalid consume preserves data");
    check(buffer_consume(&buffer, 5) == BUFFER_OK, "consume all bytes");
    check(buffer.length == 0 && buffer.capacity == 5 && buffer.data != NULL,
          "consume all retains allocation");
    check(buffer_append(&buffer, "NEW", 3) == BUFFER_OK, "reuse emptied buffer");
    check(buffer.length == 3 && memcmp(buffer.data, "NEW", 3) == 0,
          "empty buffer accepts new data");
    buffer_destroy(&buffer);
}

static void test_capacity_rejection_is_atomic(void)
{
    ByteBuffer buffer = {0};

    check(buffer_init(&buffer, 4) == BUFFER_OK, "initialize capacity buffer");
    check(buffer_append(&buffer, "abc", 3) == BUFFER_OK, "append below capacity");
    check(buffer_append(&buffer, "xy", 2) == BUFFER_LIMIT_EXCEEDED,
          "reject append instead of partially copying");
    check(buffer_append(&buffer, "x", SIZE_MAX) == BUFFER_LIMIT_EXCEEDED,
          "reject oversized length before arithmetic overflow or copy");
    check(buffer.length == 3 && buffer.capacity == 4 && memcmp(buffer.data, "abc", 3) == 0,
          "rejected appends preserve length and data");
    check(buffer_append(&buffer, "d", 1) == BUFFER_OK, "allow exact capacity");
    check(buffer_append(&buffer, "e", 1) == BUFFER_LIMIT_EXCEEDED, "reject full buffer");
    check(buffer.length == 4 && memcmp(buffer.data, "abcd", 4) == 0,
          "full buffer remains intact");
    check(buffer_append(&buffer, NULL, 0) == BUFFER_OK && buffer.length == 4,
          "zero-byte append succeeds even when full");
    buffer_destroy(&buffer);
}

static void test_invalid_arguments(void)
{
    ByteBuffer buffer = {0};

    check(buffer_init(NULL, 4) == BUFFER_INVALID_ARGUMENT, "reject NULL init");
    check(buffer_init(&buffer, 0) == BUFFER_INVALID_ARGUMENT, "reject zero capacity");
    check(buffer.data == NULL && buffer.length == 0 && buffer.capacity == 0,
          "failed initialization keeps zero state");
    check(buffer_append(NULL, "x", 1) == BUFFER_INVALID_ARGUMENT, "reject NULL append");
    check(buffer_consume(NULL, 0) == BUFFER_INVALID_ARGUMENT, "reject NULL consume");
    check(buffer_append(&buffer, "x", 1) == BUFFER_INVALID_ARGUMENT,
          "reject append before initialization");
    check(buffer_consume(&buffer, 0) == BUFFER_INVALID_ARGUMENT,
          "reject consume before initialization");
    buffer_destroy(NULL);
    buffer_destroy(&buffer);

    check(buffer_init(&buffer, 4) == BUFFER_OK, "initialize argument buffer");
    check(buffer_append(&buffer, NULL, 1) == BUFFER_INVALID_ARGUMENT,
          "reject NULL source for nonempty append");
    check(buffer_append(&buffer, NULL, 0) == BUFFER_OK && buffer.length == 0,
          "allow empty append with NULL source");
    check(buffer_consume(&buffer, 1) == BUFFER_INVALID_ARGUMENT,
          "reject consume from empty buffer");
    buffer_destroy(&buffer);
    check(buffer_append(&buffer, "x", 1) == BUFFER_INVALID_ARGUMENT,
          "reject append after destroy");
    check(buffer_consume(&buffer, 0) == BUFFER_INVALID_ARGUMENT,
          "reject consume after destroy");
}

static void test_append_from_owned_storage(void)
{
    ByteBuffer buffer = {0};

    check(buffer_init(&buffer, 8) == BUFFER_OK, "initialize alias buffer");
    check(buffer_append(&buffer, "abcd", 4) == BUFFER_OK, "append original bytes");
    check(buffer_append(&buffer, buffer.data + 1, 3) == BUFFER_OK,
          "append bytes already owned by buffer");
    check(buffer.length == 7 && memcmp(buffer.data, "abcdbcd", 7) == 0,
          "owned source is copied correctly");
    buffer_destroy(&buffer);
}

static void test_configured_capacity(void)
{
    ByteBuffer buffer = {0};
    unsigned char chunk[1024];
    memset(chunk, 0xa5, sizeof(chunk));

    check(buffer_init(&buffer, CHAT_INPUT_BUFFER_CAPACITY) == BUFFER_OK,
          "initialize planned network input capacity");
    while (buffer.length < buffer.capacity) {
        size_t remaining = buffer.capacity - buffer.length;
        size_t chunk_length = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        check(buffer_append(&buffer, chunk, chunk_length) == BUFFER_OK,
              "append repeated chunks to configured limit");
    }
    check(buffer.length == CHAT_INPUT_BUFFER_CAPACITY,
          "reach configured capacity exactly");
    for (size_t offset = 0; offset < buffer.length; ++offset) {
        check(buffer.data[offset] == 0xa5, "all received bytes preserved at limit");
    }
    check(buffer_append(&buffer, chunk, 1) == BUFFER_LIMIT_EXCEEDED,
          "enforce configured input limit");
    buffer_destroy(&buffer);
}

static void test_repeated_lifecycle(void)
{
    ByteBuffer buffer = {0};

    for (size_t iteration = 0; iteration < 1000; ++iteration) {
        check(buffer_init(&buffer, 16) == BUFFER_OK, "repeat init");
        check(buffer_append(&buffer, "abc", 3) == BUFFER_OK, "repeat append");
        check(buffer_consume(&buffer, 1) == BUFFER_OK, "repeat consume");
        check(buffer.length == 2 && memcmp(buffer.data, "bc", 2) == 0,
              "repeat lifecycle preserves remaining bytes");
        buffer_destroy(&buffer);
        check(buffer.data == NULL && buffer.length == 0 && buffer.capacity == 0,
              "repeat lifecycle returns to zero state");
    }
}

int main(void)
{
    test_init_and_destroy();
    test_binary_chunks();
    test_consume_and_reuse();
    test_capacity_rejection_is_atomic();
    test_invalid_arguments();
    test_append_from_owned_storage();
    test_configured_capacity();
    test_repeated_lifecycle();
    puts("8 buffer tests passed.");
    return EXIT_SUCCESS;
}
