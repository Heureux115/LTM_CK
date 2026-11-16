#ifndef TCP_CHAT_LISTENER_H
#define TCP_CHAT_LISTENER_H

#include <stdint.h>
#include <netinet/in.h>

/* Mở socket TCP IPv4 để nghe; trả fd do caller sở hữu, hoặc -1 và đặt errno. */
int listener_open(const char *host, uint16_t port, int backlog);
/* Lấy một kết nối đang chờ; trả client fd riêng, hoặc -1 (EAGAIN nếu hết). */
int listener_accept(int listener_fd, struct sockaddr_in *peer_address);

#endif
