#ifndef PROXY_H
#define PROXY_H

struct proxy_connection
{
    int client_fd;
    int upstream_fd;
};
int proxy_handle_client(int client_fd);

#endif
