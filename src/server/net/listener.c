/* Bật khai báo accept4() của Linux trong sys/socket.h. */
#define _GNU_SOURCE

#include "server/net/listener.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

int listener_open(const char *host, uint16_t port, int backlog)
{
    if (host == NULL || backlog <= 0) {
        errno = EINVAL;
        return -1;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    /* Port trên mạng dùng network byte order; host là IPv4 dạng chữ. */
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    /* Listener không chặn luồng và không rò fd sang chương trình exec khác. */
    int listener_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listener_fd == -1) {
        return -1;
    }

    /* SO_REUSEADDR hỗ trợ restart; bind gắn địa chỉ, listen mở hàng đợi chờ. */
    int reuse_address = 1;
    if (setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) == -1 ||
        bind(listener_fd, (const struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(listener_fd, backlog) == -1) {
        /* close() cũng có thể đổi errno: giữ lỗi gốc cho main/perror(). */
        int saved_errno = errno;
        close(listener_fd);
        errno = saved_errno;
        return -1;
    }

    return listener_fd;
}

int listener_accept(int listener_fd, struct sockaddr_in *peer_address)
{
    if (peer_address == NULL) {
        errno = EINVAL;
        return -1;
    }

    socklen_t address_size = sizeof(*peer_address);
    /* Client fd mới cũng phải non-blocking; không giả định kế thừa từ listener. */
    return accept4(listener_fd, (struct sockaddr *)peer_address, &address_size,
                   SOCK_NONBLOCK | SOCK_CLOEXEC);
}
