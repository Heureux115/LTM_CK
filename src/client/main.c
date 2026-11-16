#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client/transport.h"
#include "shared/limits.h"

static void print_usage(const char *prog_name)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s                     Run interactive mode (connects to %s:%d)\n"
            "  %s <host> <port>       Run interactive mode with custom host and port\n"
            "  %s '<JSON body>'       One-shot mode: send single frame and print response\n",
            prog_name, CHAT_DEFAULT_HOST, CHAT_DEFAULT_PORT,
            prog_name, prog_name);
}

/* Hỗ trợ cả chế độ tương tác stdin/socket và chế độ gửi một frame rồi thoát. */
int main(int argc, char **argv)
{
    /* 1. Không có tham số: chế độ tương tác mặc định */
    if (argc == 1) {
        ClientTransportResult res = client_interactive_loop(CHAT_DEFAULT_HOST, CHAT_DEFAULT_PORT,
                                                            STDIN_FILENO, STDOUT_FILENO);
        if (res != CLIENT_TRANSPORT_OK) {
            fprintf(stderr, "Client interactive session ended with error: %s\n",
                    client_transport_result_name(res));
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    /* 2. Một tham số: kiểm tra help hoặc one-shot JSON body */
    if (argc == 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }

        /* Chế độ one-shot (tương thích ngược) */
        ByteBuffer reply = {0};
        size_t body_length = strlen(argv[1]);
        printf("Sending one frame to %s:%d\n", CHAT_DEFAULT_HOST, CHAT_DEFAULT_PORT);
        ClientTransportResult result = client_roundtrip(argv[1], body_length, &reply);
        if (result != CLIENT_TRANSPORT_OK) {
            fprintf(stderr, "Client failed: %s\n", client_transport_result_name(result));
            return EXIT_FAILURE;
        }

        printf("Response (%zu bytes): ", reply.length);
        if (fwrite(reply.data, 1, reply.length, stdout) != reply.length) {
            buffer_destroy(&reply);
            fputs("Cannot print reply\n", stderr);
            return EXIT_FAILURE;
        }
        putchar('\n');
        buffer_destroy(&reply);
        return EXIT_SUCCESS;
    }

    /* 3. Hai tham số: host và port cho chế độ tương tác */
    if (argc == 3) {
        const char *host = argv[1];
        int port_val = atoi(argv[2]);
        if (port_val <= 0 || port_val > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
        ClientTransportResult res = client_interactive_loop(host, (uint16_t)port_val,
                                                            STDIN_FILENO, STDOUT_FILENO);
        if (res != CLIENT_TRANSPORT_OK) {
            fprintf(stderr, "Client interactive session ended with error: %s\n",
                    client_transport_result_name(res));
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
