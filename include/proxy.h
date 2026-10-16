#ifndef PROXY_H
#define PROXY_H
#include <stdbool.h>
struct proxy_connection
{
    int client_fd;
    int upstream_fd;
    void *upstream_ssl;
    
    ssize_t (*read)(struct proxy_connection *conn, void *buf, size_t len);
    ssize_t (*write)(struct proxy_connection *conn, void *buf, size_t len);
};
int proxy_handle_client(int client_fd);

#endif
