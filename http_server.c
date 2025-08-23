#include "client.c"
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdbool.h>

#define PORT 8080
#define CRLF "\r\n"

int main() {

    /* declare variables */
    int tcp_socket = 0;
    int rc = 0;
    int rt = 1;
    int client_socket = 0;
    struct sockaddr_in bind_addr;
    const bool off = true;
    memset(&bind_addr, 0, sizeof(bind_addr));

    /* starting the TCP connection */
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket == -1) {
        perror("socket");
        goto close;
    }

    printf("Socket created successfully\n");

    // socket options for reusing port in case client close the connection -> dont care if it fails
    (void)setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &off, sizeof(off));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port = htons(PORT);
    rc = bind(tcp_socket, (const struct sockaddr *)&bind_addr,
              sizeof(bind_addr));
    if (rc < 0) {
        perror("bind");
        rt = 1;
        goto close;
    }
    printf("Socket bound successfully\n");

    rc = listen(tcp_socket, SOMAXCONN);
    if (rc < 0) {
        perror("listen");
        rt = 1;
        goto close;
    }
    printf("Socket listening successfully\n");

    // loop to handle client requests
    for (;;) {
        printf("waiting for client connection...\n");
        client_socket =
            accept(tcp_socket, NULL, NULL); // accept all client connection
        if (client_socket < 0) {
            perror("accept");
            rt = 1;
            goto close;
        }
        printf("client connected successfully\n");
        rc = client(client_socket); // handle client request
    }
// close the socket connection
close:
    close(tcp_socket);
    return rt;
}
