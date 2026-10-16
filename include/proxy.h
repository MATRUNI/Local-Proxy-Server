#ifndef PROXY_H
#define PROXY_H
#include <stdbool.h>
struct proxy_connection
{
    int client_fd;
    int upstream_fd;
    bool is_https;
};
int proxy_handle_client(int client_fd);

#endif
