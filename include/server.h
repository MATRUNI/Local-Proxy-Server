#ifndef SERVER_H
#define SERVER_H

struct proxy_server
{
    int listen_fd;
    int port;
};

int server_init(struct proxy_server *server, int port);

int server_accept(struct proxy_server *server);

void server_destroy(struct proxy_server *server);

#endif
