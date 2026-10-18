#define _GNU_SOURCE
#include <stdio.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "server.h"
#include "proxy.h"
#include "logger.h"
#include "tls.h"
#include "config.h"

void *proxy_thead_worker(void *arg)
{
    int client_fd = *(int *)arg;
    free(arg);
    set_socket_timeout(client_fd);
    proxy_handle_client(client_fd);
    close(client_fd);
    return NULL;
}

int main(int argc,char *argv[])
{
    setbuf(stdout, NULL);
    signal(SIGPIPE, SIG_IGN);
    if(init_logger()==1)
    {
        printf("Failed to initialise logger.\n");
    }
    if(tls_init()!=0)
    {
        write_log("Failed to initialize TLS");
        return 1;
    }
    struct proxy_config config = {0};
    config.port = 17777;
    config.listen_ip = "127.0.0.1";
    config.verbose = false;
    config.mode = MODE_SERVER;

    if(parse_config(argc,argv,&config)==1)
    {
        write_log("Invalid input");
        return 1;
    }
    switch (config.mode)
    {
        case MODE_LOGS:
            get_logs();
            return 0;

        case MODE_HELP:
            printf("Usage: ./api -p <port> -m <server|stress|logs>\n");
            return 0;

        case MODE_STRESS:
            printf("Stress test mode not implemented yet!\n");
            return 0;

        case MODE_SERVER:
        default:
            break;
    }

    if (config.verbose)
    {
        printf("Verbose mode: ON\n");
    }

    int reuse=1;

    struct proxy_server server={0};
    if(server_init(&server,config.port,config.listen_ip)==1)
    {
        return 1;   
    }
    setsockopt(server.listen_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    write_log("Server it listening to %s:%d",config.listen_ip,config.port);
    pthread_t thread;
    while(1)
    {
        int client_fd = server_accept(&server);
        if(client_fd==-1)
        {
            continue;
        }
        int *client_fd_ptr = malloc(sizeof(int));
        if(client_fd_ptr==NULL)
        {
            write_log("Thread allocation for client failed.");
            close(client_fd);
            continue;
        }
        *client_fd_ptr = client_fd;

        pthread_attr_t attr;
        pthread_attr_init(&attr);

        pthread_attr_setstacksize(&attr,THREAD_STACK_SIZE);
        
        if(pthread_create(&thread,&attr,proxy_thead_worker,client_fd_ptr)!=0)
        {
            write_log("Out of threads.");
            free(client_fd_ptr);
            close(client_fd);
            continue;
        }
        pthread_attr_destroy(&attr);

        pthread_detach(thread);
    }
    server_destroy(&server);

    return 0;
}