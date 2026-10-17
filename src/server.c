#include "server.h"
#include <sys/socket.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>

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
    if(listen(fd,10)==-1)
    {
        perror("listen");
        close(fd);
        return 1;
    }
    server->listen_fd=fd;
    server->port=port;
    return 0;
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
