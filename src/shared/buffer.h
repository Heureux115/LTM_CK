#ifndef TCP_CHAT_BUFFER_H
#define TCP_CHAT_BUFFER_H

#include <stddef.h>

/* Vùng nhớ byte có dung lượng cố định; khởi tạo struct bằng {0} trước buffer_init(). */
typedef struct {
    unsigned char *data; /* Byte thô do buffer sở hữu, không phải chuỗi kết thúc bằng '\0'. */
    size_t length;        /* Số byte hợp lệ đang lưu. */
    size_t capacity;      /* Tổng số byte có thể lưu. */
} ByteBuffer;

typedef enum {
    BUFFER_OK = 0,
    BUFFER_INVALID_ARGUMENT,
    BUFFER_LIMIT_EXCEEDED,
    BUFFER_ALLOCATION_FAILED
} BufferResult;

/* Cấp phát vùng nhớ cho buffer đang ở trạng thái zero. */
BufferResult buffer_init(ByteBuffer *buffer, size_t capacity);
/* Sao chép byte vào cuối; nếu không đủ chỗ, nội dung cũ không đổi. */
BufferResult buffer_append(ByteBuffer *buffer, const void *data, size_t length);
/* Bỏ các byte đầu đã xử lý và dồn phần còn lại về đầu. */
BufferResult buffer_consume(ByteBuffer *buffer, size_t length);
/* Giải phóng vùng nhớ và đặt lại struct về trạng thái zero. */
void buffer_destroy(ByteBuffer *buffer);

#endif
