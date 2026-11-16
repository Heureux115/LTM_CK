#include "shared/buffer.h"

#include <stdlib.h>
#include <string.h>

/* Chỉ nhận struct zero để không làm mất con trỏ của vùng nhớ đã cấp phát. */
BufferResult buffer_init(ByteBuffer *buffer, size_t capacity)
{
    if (buffer == NULL || capacity == 0) {
        return BUFFER_INVALID_ARGUMENT;
    }

    if (buffer->data != NULL || buffer->length != 0 || buffer->capacity != 0) {
        return BUFFER_INVALID_ARGUMENT;
    }

    unsigned char *data = malloc(capacity);
    if (data == NULL) {
        return BUFFER_ALLOCATION_FAILED;
    }

    buffer->data = data;
    buffer->length = 0;
    buffer->capacity = capacity;
    return BUFFER_OK;
}

BufferResult buffer_append(ByteBuffer *buffer, const void *data, size_t length)
{
    if (buffer == NULL || buffer->data == NULL || buffer->length > buffer->capacity) {
        return BUFFER_INVALID_ARGUMENT;
    }

    if (data == NULL && length != 0) {
        return BUFFER_INVALID_ARGUMENT;
    }

    /* Dùng phép trừ thay vì cộng length để tránh tràn size_t. */
    if (length > buffer->capacity - buffer->length) {
        return BUFFER_LIMIT_EXCEEDED;
    }

    if (length != 0) {
        /* memmove vẫn đúng nếu nguồn nằm trong chính buffer này. */
        memmove(buffer->data + buffer->length, data, length);
        buffer->length += length;
    }

    return BUFFER_OK;
}

BufferResult buffer_consume(ByteBuffer *buffer, size_t length)
{
    if (buffer == NULL || buffer->data == NULL || buffer->length > buffer->capacity) {
        return BUFFER_INVALID_ARGUMENT;
    }

    if (length > buffer->length) {
        return BUFFER_INVALID_ARGUMENT;
    }

    if (length != 0) {
        size_t remaining = buffer->length - length;
        if (remaining != 0) {
            /* Dồn các byte chưa xử lý về đầu để tái sử dụng chỗ trống. */
            memmove(buffer->data, buffer->data + length, remaining);
        }
        buffer->length = remaining;
    }

    return BUFFER_OK;
}

void buffer_destroy(ByteBuffer *buffer)
{
    if (buffer == NULL) {
        return;
    }

    /* Sau destroy, caller có thể buffer_init() lại cùng struct. */
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}
