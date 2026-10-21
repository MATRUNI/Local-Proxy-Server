#ifndef PROXY_H
#define PROXY_H
#include <stdbool.h>

#define PROXY_DONE   0   // Request completed successfully
#define PROXY_ERROR  1   // Error occurred or client disconnected
#define PROXY_AGAIN  2   // Partial request (EAGAIN); keep session alive!

struct proxy_connection
{
    int client_fd;
    int upstream_fd;
    void *upstream_ssl;
    int status_code;
    size_t body_bytes_sent;
    ssize_t (*read)(struct proxy_connection *conn, void *buf, size_t len);
    ssize_t (*write)(struct proxy_connection *conn, void *buf, size_t len);
};
enum session_state {
    SESSION_READING_HEADERS,
    SESSION_PROCESSING,
    SESSION_DONE
};

struct client_session {
    int client_fd;
    enum session_state state;
    char *buffer;
    size_t total;
    size_t capacity;
};

struct client_session *session_create(int client_fd);

void session_free(struct client_session *session);

int proxy_handle_client(struct client_session *session);

#endif
