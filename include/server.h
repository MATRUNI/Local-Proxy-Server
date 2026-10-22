#ifndef SERVER_H
#define SERVER_H

struct proxy_server
{
    int listen_fd;
    int port;
};

int server_init(struct proxy_server *server, int port, const char *ip);

int server_accept(struct proxy_server *server);

int set_socket_non_blocking(int fd);

int set_socket_blocking(int fd);

void set_socket_timeout(int fd);

void server_destroy(struct proxy_server *server);

#endif
