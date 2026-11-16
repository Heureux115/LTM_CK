#ifndef TCP_CHAT_FRAME_H
#define TCP_CHAT_FRAME_H

#include <stddef.h>

#include "shared/buffer.h"

/* Chỉ nhìn vào input buffer: view hết hiệu lực sau append/consume/destroy. */
typedef struct {
    const unsigned char *body;
    size_t length;
} FrameView;

typedef enum {
    FRAME_OK = 0,
    FRAME_NEED_MORE,
    FRAME_INVALID_LENGTH,
    FRAME_BUFFER_FULL,
    FRAME_INVALID_ARGUMENT
} FrameResult;

/* Ghi header 4 byte big-endian và body vào output, hoặc giữ nguyên nếu lỗi. */
FrameResult frame_encode(ByteBuffer *output, const void *body, size_t length);
/* Chỉ đọc frame đầu; FRAME_NEED_MORE nghĩa là phải chờ thêm byte từ TCP. */
FrameResult frame_peek(const ByteBuffer *input, FrameView *view);
/* Bỏ đúng frame đầu đã đủ; các frame sau vẫn nằm trong input. */
FrameResult frame_consume(ByteBuffer *input);

#endif
