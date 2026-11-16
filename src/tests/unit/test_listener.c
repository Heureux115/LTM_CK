/* Kiểm tra socket TCP loopback thật, không cần chạy binary chat-server. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "server/net/listener.h"
#include "shared/limits.h"

static void check(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void test_invalid_arguments(void)
{
    errno = 0;
    check(listener_open(NULL, 0, CHAT_LISTEN_BACKLOG) == -1 && errno == EINVAL,
          "null host is rejected");
    errno = 0;
    check(listener_open("invalid address", 0, CHAT_LISTEN_BACKLOG) == -1 && errno == EINVAL,
          "invalid IPv4 address is rejected");
    errno = 0;
    check(listener_open(CHAT_DEFAULT_HOST, 0, 0) == -1 && errno == EINVAL,
          "nonpositive backlog is rejected");
}

static void test_listener_configuration(void)
{
    int listener_fd = listener_open(CHAT_DEFAULT_HOST, 0, CHAT_LISTEN_BACKLOG);
    check(listener_fd >= 0, "open loopback listener on an ephemeral port");

    struct sockaddr_in address = {0};
    socklen_t address_size = sizeof(address);
    check(getsockname(listener_fd, (struct sockaddr *)&address, &address_size) == 0,
          "query bound listener address");
    check(address.sin_family == AF_INET &&
              address.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
              ntohs(address.sin_port) != 0,
          "listener binds loopback and receives an ephemeral port");

    int status_flags = fcntl(listener_fd, F_GETFL);
    check(status_flags != -1 && (status_flags & O_NONBLOCK) != 0,
          "listening socket is nonblocking");
    int descriptor_flags = fcntl(listener_fd, F_GETFD);
    check(descriptor_flags != -1 && (descriptor_flags & FD_CLOEXEC) != 0,
          "listening socket is close-on-exec");

    int reuse_address = 0;
    socklen_t option_size = sizeof(reuse_address);
    check(getsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
                     &reuse_address, &option_size) == 0 && reuse_address == 1,
          "address reuse option is enabled");

    errno = 0;
    check(listener_open(CHAT_DEFAULT_HOST, ntohs(address.sin_port),
                        CHAT_LISTEN_BACKLOG) == -1 && errno == EADDRINUSE,
          "second listener cannot claim the same address and port");
    check(close(listener_fd) == 0, "close listening socket");
}

static void test_accept_connection(void)
{
    int listener_fd = listener_open(CHAT_DEFAULT_HOST, 0, CHAT_LISTEN_BACKLOG);
    check(listener_fd >= 0, "open listener for accept test");

    struct sockaddr_in bound_address = {0};
    socklen_t address_size = sizeof(bound_address);
    check(getsockname(listener_fd, (struct sockaddr *)&bound_address, &address_size) == 0,
          "query listener port for client connect");

    struct sockaddr_in peer_address = {0};
    errno = 0;
    check(listener_accept(listener_fd, &peer_address) == -1 &&
              (errno == EAGAIN || errno == EWOULDBLOCK),
          "nonblocking accept reports empty queue");
    errno = 0;
    check(listener_accept(listener_fd, NULL) == -1 && errno == EINVAL,
          "null peer address is rejected");

    int client_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(client_fd >= 0, "create test client socket");
    check(connect(client_fd, (const struct sockaddr *)&bound_address,
                  sizeof(bound_address)) == 0,
          "client completes TCP connection to listener");

    int accepted_fd = listener_accept(listener_fd, &peer_address);
    check(accepted_fd >= 0, "accept queued TCP connection");
    check(peer_address.sin_family == AF_INET &&
              peer_address.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
              ntohs(peer_address.sin_port) != 0,
          "accepted connection records loopback peer");
    int status_flags = fcntl(accepted_fd, F_GETFL);
    check(status_flags != -1 && (status_flags & O_NONBLOCK) != 0,
          "accepted socket is nonblocking");
    int descriptor_flags = fcntl(accepted_fd, F_GETFD);
    check(descriptor_flags != -1 && (descriptor_flags & FD_CLOEXEC) != 0,
          "accepted socket is close-on-exec");

    check(close(accepted_fd) == 0, "close accepted socket");
    check(close(client_fd) == 0, "close test client socket");
    check(close(listener_fd) == 0, "close accept test listener");
}

int main(void)
{
    test_invalid_arguments();
    test_listener_configuration();
    test_accept_connection();
    puts("3 listener tests passed.");
    return EXIT_SUCCESS;
}
