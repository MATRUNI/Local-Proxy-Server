#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <signal.h>

#include "server.h"
#include "proxy.h"
#include "logger.h"


int main(int argc,char *argv[])
{
    setbuf(stdout, NULL);
    signal(SIGPIPE, SIG_IGN);
    int port=1777;
    if(init_logger()==1)
    {
        printf("Failed to initialise logger.\n");
    }
    for(int i=1;i<argc;i++)
    {
        if (strcasecmp(argv[i], "-p") == 0)
        {
            if (i + 1 < argc)
            {
                char *end;
                long value=strtol(argv[++i],&end,10);
                if(*end!='\0' || value<1 || value>65535)
                {
                    write_log("Invalid port.");
                    close_logger();
                    return 1;
                }
                port = (int)value;
            }
        }
        else if (strcasecmp(argv[i], "-n") == 0)
        {
            if (i + 1 < argc)
            {
                printf("Name: %s\n", argv[++i]);
            }
        }
        else if (strcasecmp(argv[i],"logs")==0)
        {
            if(get_logs()==1)
            {
                printf("Error while retrieving logs.\n");
                return 1;
            }
            return 0;
        }
        else
        {
            write_log("Invalid arguments.");
            close_logger();
            return 1;
        }    
    }
    int reuse=1;

    struct proxy_server server={0};
    if(server_init(&server,port)==1)
    {
        return 1;   
    }
    setsockopt(server.listen_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    write_log("Server it listening to 127.0.0.1:%d",port);
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
            // break;
            continue;
        }
        close(client_fd);
    }
    server_destroy(&server);

    return 0;
}