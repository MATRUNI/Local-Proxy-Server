#include "server.h"
#include <sys/socket.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>

#include "config.h"
#include "logger.h"

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

int check_socket_connected(int fd)
{
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
    {
        return -1;
    }
    return (error == 0) ? 0 : -1;
}

int send_all(int fd, const char *buffer, size_t total)
{
    size_t s = 0;
    while (s < total)
    {
        ssize_t sent = send(fd, buffer + s, total - s, 0);
        
        if (sent == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                write_log(LOG_WARN, "Client write timed out (client not consuming data).");
            }
            else if (errno == EPIPE || errno == ECONNRESET) {
                write_log(LOG_WARN, "Client disconnected prematurely (broken pipe).");
            }
            else {
                perror("send");
            }
            return 1;
        }
        s += sent;
    }
    return 0;
}

