#ifndef TCP_CHAT_CLIENT_TRANSPORT_H
#define TCP_CHAT_CLIENT_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

#include "shared/buffer.h"

typedef enum {
    CLIENT_TRANSPORT_OK = 0,
    CLIENT_TRANSPORT_INVALID_BODY,
    CLIENT_TRANSPORT_ALLOCATION_ERROR,
    CLIENT_TRANSPORT_CONNECT_ERROR,
    CLIENT_TRANSPORT_IO_ERROR,
    CLIENT_TRANSPORT_PROTOCOL_ERROR,
    CLIENT_TRANSPORT_TIMEOUT
} ClientTransportResult;

/* Chờ connection.ready hợp lệ, gửi một frame, nhận một response; reply bắt đầu {0}. */
ClientTransportResult client_roundtrip(const char *body, size_t length, ByteBuffer *reply);
/* Bản chọn endpoint/timeout để kiểm thử; timeout áp dụng cho cả lần roundtrip. */
ClientTransportResult client_roundtrip_to(const char *host, uint16_t port,
                                           const char *body, size_t length,
                                           int timeout_ms, ByteBuffer *reply);
/* Khởi chạy vòng lặp tương tác client: poll đồng thời in_fd và socket, tự động pong và tăng request_id. */
ClientTransportResult client_interactive_loop(const char *host, uint16_t port,
                                              int in_fd, int out_fd);
const char *client_transport_result_name(ClientTransportResult result);

#endif
