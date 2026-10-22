#include "server.h"
#include <sys/socket.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>

#include "config.h"

int server_init(struct proxy_server *server, int port,const char *ip)
{
    if(server==NULL) return 1;
    int fd = socket(AF_INET,SOCK_STREAM,0);
    if(fd==-1)
    {
        perror("socket");
        return 1;
    }
    int reuse=1;
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    struct sockaddr_in address={0};
    address.sin_family=AF_INET;
    address.sin_port=htons(port);
    if (inet_pton(AF_INET, ip, &address.sin_addr) <= 0)
    {
        fprintf(stderr, "Invalid IP address: %s\n", ip);
        close(fd);
        return 1;
    }

    if(bind(fd,(struct sockaddr *)&address,sizeof(address))==-1)
    {
        perror("bind");
        close(fd);
        return 1;
    }
    if(listen(fd,SOMAXCONN)==-1)
    {
        perror("listen");
        close(fd);
        return 1;
    }
    server->listen_fd=fd;
    server->port=port;
    return 0;
}

void set_socket_timeout(int fd)
{
    struct timeval timeout = {
        .tv_sec = PROXY_TIMEOUT_SEC,
        .tv_usec = 0
    };
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    return;
}

int set_socket_non_blocking(int fd)
{
    int flags = fcntl(fd,F_GETFL);
    if(flags == -1) return -1;
    return fcntl(fd,F_SETFL,flags | O_NONBLOCK);
}

int set_socket_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if(flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

int server_accept(struct proxy_server *server)
{
    if(server==NULL) return -1;
    return accept(server->listen_fd,NULL,NULL);
}

void server_destroy(struct proxy_server *server)
{
    if(server==NULL) return;
    if(server->listen_fd!=-1)
        close(server->listen_fd);
    server->listen_fd=-1;
    server->port=0;
}
