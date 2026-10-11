#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>

#include "server.h"
#include "proxy.h"
#include "logger.h"


int main(int argc,char *argv[])
{
    FILE *logger = NULL;
    setbuf(stdout, NULL);
    int port=1777;
    int logger_active=0;
    if(init_logger(&logger)==1)
    {
        printf("Logger initialization failed.\n");
        logger_active=1;
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
                    write_log(logger,"Invalid port.");
                    close_logger(&logger);
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
            if(get_logs(logger)==1)
            {
                printf("Error while retrieving logs.\n");
                return 1;
            }
        }
        else
        {
            write_log(logger,"Invalid arguments.");
            close_logger(&logger);
            return 1;
        }    
    }
    int reuse=1;
    char line[50];
    snprintf(line,sizeof(line),"Server it listening to 127.0.0.1:%d",port);
    write_log(logger,line);
    
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