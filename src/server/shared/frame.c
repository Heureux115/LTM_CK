#include "shared/frame.h"

#include <stdint.h>
#include <string.h>

#include "shared/limits.h"

FrameResult frame_encode(ByteBuffer *output, const void *body, size_t length)
{
    if (output == NULL || output->data == NULL || output->length > output->capacity) {
        return FRAME_INVALID_ARGUMENT;
    }

    if (length == 0 || length > CHAT_MAX_FRAME_BODY_BYTES) {
        return FRAME_INVALID_LENGTH;
    }

    if (body == NULL) {
        return FRAME_INVALID_ARGUMENT;
    }

    /* Kiểm tra đủ chỗ cho cả header và body trước khi ghi một byte nào. */
    size_t available = output->capacity - output->length;
    if (available < CHAT_FRAME_HEADER_BYTES || length > available - CHAT_FRAME_HEADER_BYTES) {
        return FRAME_BUFFER_FULL;
    }

    /* Byte cao đứng trước: cùng một frame sẽ đọc đúng trên mọi máy. */
    uint32_t body_length = (uint32_t)length;
    unsigned char *destination = output->data + output->length;
    destination[0] = (unsigned char)(body_length >> 24);
    destination[1] = (unsigned char)(body_length >> 16);
    destination[2] = (unsigned char)(body_length >> 8);
    destination[3] = (unsigned char)body_length;
    memmove(destination + CHAT_FRAME_HEADER_BYTES, body, length);
    output->length += CHAT_FRAME_HEADER_BYTES + length;

    return FRAME_OK;
}

FrameResult frame_peek(const ByteBuffer *input, FrameView *view)
{
    if (view == NULL) {
        return FRAME_INVALID_ARGUMENT;
    }

    view->body = NULL;
    view->length = 0;

    if (input == NULL || input->data == NULL || input->length > input->capacity) {
        return FRAME_INVALID_ARGUMENT;
    }

    /* TCP có thể mới chuyển một phần header 4 byte. */
    if (input->length < CHAT_FRAME_HEADER_BYTES) {
        return FRAME_NEED_MORE;
    }

    /* Ghép bốn byte theo big-endian, không cast con trỏ gây lỗi alignment. */
    uint32_t body_length = ((uint32_t)input->data[0] << 24) |
                           ((uint32_t)input->data[1] << 16) |
                           ((uint32_t)input->data[2] << 8) |
                           (uint32_t)input->data[3];
    if (body_length == 0 || body_length > CHAT_MAX_FRAME_BODY_BYTES) {
        return FRAME_INVALID_LENGTH;
    }

    size_t frame_length = CHAT_FRAME_HEADER_BYTES + (size_t)body_length;
    /* Đủ header nhưng body chưa tới hết thì giữ nguyên buffer. */
    if (input->length < frame_length) {
        return FRAME_NEED_MORE;
    }

    /* View trỏ vào buffer gốc; không cấp phát hoặc sao chép body. */
    view->body = input->data + CHAT_FRAME_HEADER_BYTES;
    view->length = body_length;
    return FRAME_OK;
}

FrameResult frame_consume(ByteBuffer *input)
{
    FrameView view;
    FrameResult result = frame_peek(input, &view);
    if (result != FRAME_OK) {
        return result;
    }

    if (buffer_consume(input, CHAT_FRAME_HEADER_BYTES + view.length) != BUFFER_OK) {
        return FRAME_INVALID_ARGUMENT;
    }

    return FRAME_OK;
}
