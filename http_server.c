#include "file_sys.h"
#include "strings.h"
#include <fcntl.h>
#include <linux/limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRLF "\r\n"
#define SP " "

const string_view WEB_ROOT = STRING_VIEW_FROM_LITERAL("./html/");

/* Request-Line = Method SP Request-URI SP HTTP-Version CRLF */
typedef struct {
    string_view method;
    string_view uri;
    string_view version;
} http_req_line;

typedef enum http_status {
    HTTP_RES_OK = 200,
    HTTP_RES_BAD_REQUEST = 400,
    HTTP_RES_NOT_FOUND = 404,
    HTTP_RES_INTERNAL_SERVER_ERR = 500,

} http_status;

const char *http_status_to_string(http_status status) {
    switch (status) {
    case HTTP_RES_OK:
        return "OK";
    case HTTP_RES_BAD_REQUEST:
        return "Bad Request";
    case HTTP_RES_INTERNAL_SERVER_ERR:
        return "Internal Server Error";
    case HTTP_RES_NOT_FOUND:
        return "Not Found";
    default:
        return "Unknown";
    }
}

http_req_line http_req_line_init(void) {
    http_req_line line;
    memset(&line, 0, sizeof(line));
    return line;
}

http_status parse_req_line(http_req_line *req_line, const char *buf,
                           size_t len) {
    if (!buf || !req_line) {

        return HTTP_RES_INTERNAL_SERVER_ERR;
    }

    string_splits components = split_string(buf, len, SP);

    if (components.count != 3) {
        printf("ERROR: invalid request line: expected 3 components, got %zu\n",
               components.count);
        return HTTP_RES_BAD_REQUEST;
    }

    req_line->method.data = components.splits[0].data;
    req_line->method.len = components.splits[0].len;
    req_line->uri.data = components.splits[1].data;
    req_line->uri.len = components.splits[1].len;

    req_line->version.data = components.splits[2].data;
    req_line->version.len = components.splits[2].len;

    free_splits(&components);
    return HTTP_RES_OK;
}

string_view http_response_generate(char *buf, size_t buf_len,
                                   http_status status, size_t body_len) {
    string_view response;
    response.len = 0;
    memset(buf, 0, buf_len);

    response.len += sprintf(buf, "%s %d %s" CRLF, "HTTP/1.1", status,
                            http_status_to_string(status));
    response.len +=
        sprintf(buf + response.len, "Content-Length: %zu" CRLF, body_len);
    response.len += sprintf(buf + response.len, CRLF);
    response.data = buf;
    return response;
}

bool http_send_response(int socket, string_view header, string_view body) {

    ssize_t n = send(socket, header.data, header.len, MSG_MORE);
    if (n < 0) {
        perror("send()");
        return false;
    }
    if (n == 0) {
        fprintf(stderr, "send() returned 0\n");
        return false;
    }
    n = send(socket, body.data, body.len, 0);
    return true;
}

static string_view err_404 =
    STRING_VIEW_FROM_LITERAL("<p>Error 404: Not Found</p><p><a "
                             "href=\"/main.html\">Back to home</a></p>");

bool http_serve_file(int socket, string_view filename) {
    char buf[64];

    char filename_buf[PATH_MAX];
    bool return_value = true;
    int in_fd = -1;
    ssize_t result = 0;
    string_view header;
    off_t sendfile_offset = 0;
    size_t sent = 0;

    memcpy(filename_buf, WEB_ROOT.data, WEB_ROOT.len);
    memcpy(filename_buf + WEB_ROOT.len - 1, filename.data, filename.len);
    filename_buf[WEB_ROOT.len + filename.len - 1] = 0;

    fs_metadata file_metadata =
        fs_get_metadata(string_view_from_cstr(filename_buf));
    if (!file_metadata.exists) {
        (void)http_send_response(socket,
                                 http_response_generate(buf, sizeof(buf),
                                                        HTTP_RES_NOT_FOUND,
                                                        err_404.len),
                                 err_404);
        return false;
    }

    header = http_response_generate(buf, sizeof(buf), HTTP_RES_OK,
                                    file_metadata.size);

    ssize_t n = send(socket, header.data, header.len, MSG_MORE);
    if (n < 0) {
        perror("send()");
        return_value = false;
        goto cleanup;
    }
    if (n == 0) {
        fprintf(stderr, "send() returned 0\n");
        return_value = false;
        goto cleanup;
    }
    in_fd = open(filename_buf, O_RDONLY);

    if (in_fd < 0) {
        (void)http_send_response(socket,
                                 http_response_generate(buf, sizeof(buf),
                                                        HTTP_RES_NOT_FOUND,
                                                        err_404.len),
                                 err_404);
        return_value = false;
        goto cleanup;
    }

    while (sent < file_metadata.size) {
        result = sendfile(socket, in_fd, &sendfile_offset, file_metadata.size);
        if (result < 0) {
            printf("ERROR: sendfile() failed for \"%s\": %s\n", filename_buf,
                   strerror(errno));
            (void)http_send_response(
                socket,
                http_response_generate(buf, sizeof(buf),
                                       HTTP_RES_INTERNAL_SERVER_ERR,
                                       err_404.len),
                err_404);
            return_value = false;
            goto cleanup;
        }
        sent += result;
    }

cleanup:
    if (in_fd != -1) {
        close(in_fd);
    }

    return return_value;
}

void *handle_client(void *client_socket_ptr) {
    int client_socket = (int)(intptr_t)client_socket_ptr;
    ssize_t n = 0;
    int result = 0;
    char buf[1024];

    printf("\n---\n");
    for (;;) {
        memset(buf, 0, sizeof(buf));

        n = read(client_socket, buf, sizeof(buf) - 1);
        if (n < 0) {
            perror("read(client)");
            result = -1;
            break;
        }
        if (n == 0) {
            printf("connection closed gracefully!\n");
            break;
        }

        string_splits lines = split_string(buf, n, CRLF);
        if (lines.count < 1) {
            printf("ERROR: empty request\n");
            result = -1;
            break;
        }

        http_req_line req_line = http_req_line_init();
        http_status status = parse_req_line(&req_line, lines.splits[0].data,
                                            lines.splits[0].len);
        free_splits(&lines);
        if (status != HTTP_RES_OK) {
            printf("ERROR: failed to parse request line\n");
            result = -1;
            break;
        }

        printf("REQUEST: %.*s %.*s\n", (int)req_line.method.len,
               req_line.method.data, (int)req_line.uri.len, req_line.uri.data);

        string_view route_root = STRING_VIEW_FROM_LITERAL("/");
        if (string_view_equal(&req_line.uri, &route_root)) {
            if (!http_serve_file(client_socket,
                                 string_view_from_cstr("index.html"))) {
                result = -1;
                break;
            }
        } else {
            if (!http_serve_file(client_socket, req_line.uri)) {
                result = -1;
                break;
            }
        }
    }
    printf("Closing connection with result %d\n", result);
    close(client_socket);
    return NULL;
}

const int PORT = 8000;

int main(void) {

    int rc = 0;
    struct sockaddr_in bind_addr;
    int tcp_socket = 0;
    int ret = 0;
    int client_socket = 0;
    int enabled = true;
    pthread_t *threads = NULL;
    size_t threads_count = 0;
    size_t threads_capacity = 10;

    threads = (pthread_t *)calloc(threads_capacity, sizeof(pthread_t));

    if (!threads) {
        perror("calloc()");
        return 1;
    }

    fs_metadata web_root_meta = fs_get_metadata(WEB_ROOT);
    if (!web_root_meta.exists) {
        mkdir(WEB_ROOT.data, S_IEXEC | S_IWRITE | S_IREAD | S_IRGRP | S_IXGRP |
                                 S_IXOTH | S_IROTH);
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        perror("socket()");
        free(threads);
        return 1;
    }
    printf("socket creation succeeded\n");

    (void)setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled));

    bind_addr.sin_port = htons(PORT);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    rc = bind(tcp_socket, (const struct sockaddr *)&bind_addr,
              sizeof(bind_addr));
    if (rc < 0) {
        perror("bind()");
        free(threads);
        close(tcp_socket);
        return 1;
    }
    printf("bind succeeded\n");

    rc = listen(tcp_socket, SOMAXCONN);
    if (rc < 0) {
        perror("listen()");
        free(threads);
        close(tcp_socket);
        return 1;
    }
    printf("listening on http://localhost:%d/\n", PORT);

    for (;;) {
        printf("waiting for connections...\n");
        client_socket = accept(tcp_socket, NULL, NULL);
        if (client_socket < 0) {
            perror("accept()");
            continue;
        }
        printf("got a connection!\n");
        pthread_t thread;
        rc = pthread_create(&thread, NULL, handle_client,
                            (void *)(intptr_t)client_socket);
        if (rc != 0) {
            perror("pthread_create()");
            close(client_socket);

            continue;
        }
        threads[threads_count] = thread;
        ++threads_count;
        if (threads_count + 1 > threads_capacity) {
            threads_capacity = threads_capacity * 1.5f;
            pthread_t *new_threads = (pthread_t *)realloc(
                threads, threads_capacity * sizeof(pthread_t));
            if (!new_threads) {
                perror("realloc()");
                close(client_socket);
                goto exit;
            }
            threads = new_threads;
        }
    }

exit:
    for (size_t i = 0; i < threads_count; ++i) {
        pthread_join(threads[i], NULL);
    }
    free(threads);
    close(tcp_socket);
    return ret;
}
