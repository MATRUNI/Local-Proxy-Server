#ifndef PROXY_H
#define PROXY_H
#include "config.h"
#include "http.h"
#include "url.h"
#include "poll_engine.h"

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

typedef enum {
    LEVEL_CLIENT_READ_HEADERS, 
    LEVEL_UPSTREAM_SEND_REQ,   
    
    LEVEL_CLIENT_READ_BODY,    
    LEVEL_UPSTREAM_CONNECT,    
    LEVEL_UPSTREAM_READ_RESP,  

    LEVEL_CLIENT_WRITE_RESP,   
    LEVEL_COMPLETE             
} session_level_t;

struct client_session {
    struct proxy_connection con;
    struct poll_engine *engine;
    struct http_url url;
    session_level_t level;
    struct http_request req;
    char *buffer;
    size_t total;
    size_t offset;
    size_t capacity;
    char upstream_host[256];
    char upstream_port[16];
    bool upstream_is_https;
};


struct client_session *session_create(int client_fd, struct poll_engine *engine);

void session_free(struct client_session *session);

int proxy_handle_client(struct client_session *session);

#endif
