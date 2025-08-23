#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int client(int client_socket) {
    ssize_t n = 0;
    char buf[1024];
    char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 13\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "This is 3x3 👻";
    for (;;) {
        printf("\n-------------------------------------\n");
        memset(buf, 0, sizeof(buf));
        n = read(client_socket, buf, sizeof(buf) - 1);
        if (n < 0) {
            perror("read(client_socket)");
            return 1;
        }
        if (n == 0) {
            printf("Conenction closed by peer\n");
            break;
        }
        printf("Request:\n%s", buf);
        (void)write(client_socket, response, sizeof(response) - 1);
    }
    printf("-------------------------------------\n");
    return 0;
}
