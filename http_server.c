#include "file_sys.h"
#include "strings.h"
#include <fcntl.h>
#include <linux/limits.h>
#include <netinet/in.h>
#include <stdbool.h>

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CRLF "\r\n"
#define SP " "
#define WEB_ROOT                                                               \
    STRING_VIEW_FROM_LITERAL(                                                  \
        "./html/") // root directory for serving static files
#define PORT 8000  // port for HTTP server to listen on

// structure to represent HTTP request line components (Method, URI, Version)
typedef struct {
    string_view method;  // HTTP method (e.g., GET, POST)
    string_view uri;     // requested resource URI
    string_view version; // HTTP version (e.g., HTTP/1.1)
} http_req_line;

// enum for HTTP status codes
typedef enum http_status {

    HTTP_RES_OK = 200,                 // successful request
    HTTP_RES_BAD_REQUEST = 400,        // malformed request syntax
    HTTP_RES_NOT_FOUND = 404,          // requested resource not found
    HTTP_RES_INTERNAL_SERVER_ERR = 500 // server encountered an error
} http_status;

// converts HTTP status code to human-readable string
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

// initializes an empty HTTP request line structure
http_req_line http_req_line_init(void) {
    http_req_line line;
    memset(&line, 0, sizeof(line));
    return line;
}

// parses HTTP request line into method, URI, and version
// returns HTTP status indicating success or failure
http_status parse_req_line(http_req_line *req_line, const char *buf,
                           size_t len) {
    if (!buf || !req_line) {
        return HTTP_RES_INTERNAL_SERVER_ERR; // null input check
    }

    string_splits components = split_string(buf, len, SP); // split by space
    if (components.count != 3) {
        printf("error: invalid request line: expected 3 components, got %zu\n",
               components.count);
        free_splits(&components);
        return HTTP_RES_BAD_REQUEST; // request line must have exactly 3 parts
    }

    req_line->method = components.splits[0];
    req_line->uri = components.splits[1];
    req_line->version = components.splits[2];
    free_splits(&components); // release split memory
    return HTTP_RES_OK;
}

// generates HTTP response header with status and content length
string_view http_response_generate(char *buf, size_t buf_len,
                                   http_status status, size_t body_len) {
    string_view response = {.data = buf, .len = 0};
    memset(buf, 0, buf_len); // clear buffer

    response.len += sprintf(buf, "HTTP/1.1 %d %s" CRLF, status,
                            http_status_to_string(status));
    response.len +=
        sprintf(buf + response.len, "Content-Length: %zu" CRLF, body_len);
    response.len += sprintf(buf + response.len, CRLF);
    return response;
}

// sends HTTP response header and body to client socket
bool http_send_response(int socket, string_view header, string_view body) {
    ssize_t n = send(socket, header.data, header.len,
                     MSG_MORE); // send header with MSG_MORE to optimize
    if (n <= 0) {

        perror("send header");

        return false;
    }
    n = send(socket, body.data, body.len, 0); // send body
    if (n <= 0) {
        perror("send body");
        return false;
    }
    return true;
}

// static 404 error response for not found resources
static string_view err_404 =
    STRING_VIEW_FROM_LITERAL("<p>Error 404: Not Found</p><p><a "
                             "href=\"/main.html\">Back to home</a></p>");

// serves a file from WEB_ROOT directory to client socket
// handles file metadata and efficient transfer using sendfile

bool http_serve_file(int socket, string_view filename) {
    char buf[64];                // buffer for response header
    char filename_buf[PATH_MAX]; // buffer for full file path
    bool return_value = true;
    int in_fd = -1;
    ssize_t result = 0;
    string_view header;

    off_t sendfile_offset = 0;

    size_t sent = 0;

    // construct full file path: WEB_ROOT + filename
    memcpy(filename_buf, WEB_ROOT.data, WEB_ROOT.len);
    memcpy(filename_buf + WEB_ROOT.len - 1, filename.data, filename.len);
    filename_buf[WEB_ROOT.len + filename.len - 1] = '\0';

    // check if file exists and get its metadata

    fs_metadata file_metadata =
        fs_get_metadata(string_view_from_cstr(filename_buf));
    if (!file_metadata.exists) {
        http_send_response(socket,
                           http_response_generate(buf, sizeof(buf),
                                                  HTTP_RES_NOT_FOUND,
                                                  err_404.len),
                           err_404);
        return false;
    }

    // generate and send response header
    header = http_response_generate(buf, sizeof(buf), HTTP_RES_OK,
                                    file_metadata.size);
    ssize_t n = send(socket, header.data, header.len, MSG_MORE);
    if (n <= 0) {
        perror("send header");
        return_value = false;
        goto cleanup;
    }

    // open file for reading
    in_fd = open(filename_buf, O_RDONLY);
    if (in_fd < 0) {
        http_send_response(socket,
                           http_response_generate(buf, sizeof(buf),
                                                  HTTP_RES_NOT_FOUND,
                                                  err_404.len),
                           err_404);
        return_value = false;
        goto cleanup;
    }

    // transfer file content using sendfile for efficiency
    while (sent < file_metadata.size) {
        result = sendfile(socket, in_fd, &sendfile_offset,
                          file_metadata.size - sent);
        if (result < 0) {
            printf("error: sendfile failed for \"%s\": %s\n", filename_buf,
                   strerror(errno));
            http_send_response(socket,
                               http_response_generate(
                                   buf, sizeof(buf),
                                   HTTP_RES_INTERNAL_SERVER_ERR, err_404.len),
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

// handles client connection in a separate thread
// processes HTTP requests and serves files or error responses
void *handle_client(void *client_socket_ptr) {
    int client_socket = (int)(intptr_t)client_socket_ptr;
    ssize_t n = 0;
    int result = 0;
    char buf[1024];

    printf("\n---\n");
    for (;;) {
        memset(buf, 0, sizeof(buf));
        n = recv(client_socket, buf, sizeof(buf) - 1, 0); // read client request
        if (n < 0) {
            perror("recv");
            result = -1;
            break;
        }
        if (n == 0) {
            printf("connection closed by client\n");
            break;
        }

        string_splits lines =
            split_string(buf, n, CRLF); // split request by lines
        if (lines.count < 1) {
            printf("error: empty request\n");
            result = -1;
            free_splits(&lines);

            break;
        }

        http_req_line req_line = http_req_line_init();
        http_status status = parse_req_line(&req_line, lines.splits[0].data,
                                            lines.splits[0].len);
        free_splits(&lines);
        if (status != HTTP_RES_OK) {
            printf("error: failed to parse request line\n");
            result = -1;
            break;
        }

        printf("request: %.*s %.*s\n", (int)req_line.method.len,
               req_line.method.data, (int)req_line.uri.len, req_line.uri.data);

        // serve index.html for root path, otherwise serve requested file
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
    printf("closing connection with result %d\n", result);
    close(client_socket);
    return NULL;
}

int main(void) {
    int rc = 0;
    struct sockaddr_in bind_addr;
    int tcp_socket = 0;
    int ret = 0;
    int client_socket = 0;
    int enabled = 1;
    pthread_t *threads = NULL;
    size_t threads_count = 0;
    size_t threads_capacity = 10;

    // allocate memory for thread pool
    threads = (pthread_t *)calloc(threads_capacity, sizeof(pthread_t));
    if (!threads) {
        perror("calloc threads");
        return 1;
    }

    // ensure web root directory exists
    fs_metadata web_root_meta = fs_get_metadata(WEB_ROOT);
    if (!web_root_meta.exists) {
        mkdir(WEB_ROOT.data, S_IEXEC | S_IWRITE | S_IREAD | S_IRGRP | S_IXGRP |
                                 S_IXOTH | S_IROTH);
    }

    // create TCP socket
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        perror("socket");
        free(threads);
        return 1;
    }
    printf("socket creation succeeded\n");

    // enable SO_REUSEADDR to allow port reuse
    setsockopt(tcp_socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

    // configure server address
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_port = htons(PORT);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    // bind socket to address
    rc = bind(tcp_socket, (const struct sockaddr *)&bind_addr,
              sizeof(bind_addr));
    if (rc < 0) {
        perror("bind");
        free(threads);
        close(tcp_socket);
        return 1;
    }
    printf("bind succeeded\n");

    // listen for incoming connections
    rc = listen(tcp_socket, SOMAXCONN);
    if (rc < 0) {
        perror("listen");
        free(threads);
        close(tcp_socket);
        return 1;
    }

    printf("listening on http://localhost:%d/\n", PORT);

    // accept and handle client connections
    for (;;) {
        printf("waiting for connections...\n");
        client_socket = accept(tcp_socket, NULL, NULL);
        if (client_socket < 0) {
            perror("accept");
            continue;
        }
        printf("got a connection!\n");

        // create thread to handle client
        pthread_t thread;
        rc = pthread_create(&thread, NULL, handle_client,
                            (void *)(intptr_t)client_socket);
        if (rc != 0) {
            perror("pthread_create");
            close(client_socket);
            continue;
        }

        // store thread and resize pool if needed
        threads[threads_count] = thread;
        ++threads_count;

        if (threads_count + 1 > threads_capacity) {
            threads_capacity = threads_capacity * 1.5;
            pthread_t *new_threads = (pthread_t *)realloc(
                threads, threads_capacity * sizeof(pthread_t));
            if (!new_threads) {
                perror("realloc threads");
                close(client_socket);
                goto exit;
            }
            threads = new_threads;
        }
    }

exit:
    // wait for all threads to complete
    for (size_t i = 0; i < threads_count; ++i) {
        pthread_join(threads[i], NULL);
    }
    free(threads);
    close(tcp_socket);
    return ret;
}
