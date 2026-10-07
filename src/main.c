#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>

#include "server.h"
#include "proxy.h"


int main(int argc,char *argv[])
{
    setbuf(stdout, NULL);
    int port=1777;
    for(int i=1;i<argc;i++)
    {
        if (strcmp(argv[i], "-p") == 0)
        {
            if (i + 1 < argc)
            {
                char *end;
                long value=strtol(argv[++i],&end,10);
                if(*end!='\0' || value<1 || value>65535)
                {
                    printf("Invalid port.\n");
                    return 1;
                }
                port = (int)value;
            }
        }
        else if (strcmp(argv[i], "-n") == 0)
        {
            if (i + 1 < argc)
            {
                printf("Name: %s\n", argv[++i]);
            }
        }
        else
        {
            printf("Invalid arguments.\n");
            return 1;
        }    
    }
    int reuse=1;
    
    printf("Server it listening to 127.0.0.1:%d\n",port);
    
    struct proxy_server server={0};
    setsockopt(server.listen_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    if(server_init(&server,port)==1)
    {
        return 1;   
    }
    while(1)
    {
        int client_fd = server_accept(&server);
        if(client_fd==-1)
        {
            continue;
        }
        if(proxy_handle_client(client_fd)==1)
        {
            close(client_fd);
            break;
        }
        close(client_fd);
    }
    server_destroy(&server);

    return 0;
}