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
#include <fcntl.h>
#include <sys/epoll.h>

#include "server.h"
#include "proxy.h"
#include "logger.h"
#include "tls.h"
#include "config.h"

static volatile sig_atomic_t server_running = 1;
static pthread_attr_t attr;
struct worker_context {
    int id;
    int epoll_fd;
    int listen_fd;
    pthread_t thread;
};

#define MAX_EVENTS 64

static void handle_sigint(int sig)
{
    (void)sig;
    server_running = 0;
}

static void *proxy_thread_worker(void *arg)
{
    struct worker_context *self = (struct worker_context *)arg;
    struct epoll_event events[MAX_EVENTS];
    while (server_running)
    {
        int n =epoll_wait(self->epoll_fd,events,MAX_EVENTS,500);
        if(n==-1)
        {
            continue;
        }
        for(int i=0;i<n;i++)
        {
            if(events[i].data.ptr==NULL)
            {
                while (1)
                {
                    int client_fd = accept(self->listen_fd,NULL,NULL);
                    if (client_fd == -1)
                    {
                        break;
                    }
                    int res = set_socket_non_blocking(client_fd);
                    set_socket_nodelay(client_fd);
                    if(res==-1)
                    {
                        close(client_fd);
                        continue;
                    }
                    struct client_session *ses = session_create(client_fd,self->epoll_fd);
                    if(ses==NULL)
                    {
                        close(client_fd);
                        continue;
                    }
                    struct epoll_event ev = {
                        .data.ptr = ses,
                        .events = EPOLLIN | EPOLLET | EPOLLONESHOT
                    };
                    epoll_ctl(self->epoll_fd,EPOLL_CTL_ADD,client_fd,&ev);
                }
            }
            else
            {
                struct client_session *session = events[i].data.ptr;
                set_socket_timeout(session->con.client_fd);
                if(proxy_handle_client(session)!=PROXY_AGAIN)
                {
                    session_free(session);
                }
            }
        }
    }
    close(self->listen_fd);
    close(self->epoll_fd);
    return NULL;    
}

static void get_worker_thread_attr(void)
{
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
}

static void destroy_worker_thread_attr(void)
{
    pthread_attr_destroy(&attr);
}

static int spawn_worker_fleet(struct worker_context *workers, int threads,int port, const char *ip, int timeout)
{
    if(workers == NULL)
    {
        write_log(LOG_ERROR,"Fail allocate memory for threads.");
        return 1;
    }

    for(int i=0;i<threads;i++)
    {
        workers[i].id = i;
        workers[i].epoll_fd = epoll_create1(0);
        struct proxy_server srv = {0};
        if(server_init(&srv, port, ip,timeout) != 0)
        {
            write_log(LOG_ERROR, "Failed to init listener socket for worker %d", i);
            return 1;
        }
        set_socket_non_blocking(srv.listen_fd);
        workers[i].listen_fd = srv.listen_fd;
        struct epoll_event ev = {
            .data.ptr = NULL,
            .events = EPOLLIN | EPOLLET
        };
        epoll_ctl(workers[i].epoll_fd,EPOLL_CTL_ADD,workers[i].listen_fd,&ev);
        if (pthread_create(&workers[i].thread, &attr, proxy_thread_worker, &workers[i]) != 0)
        {
            write_log(LOG_ERROR, "Out of threads.");
            return 1;
        }
    }
    return 0;
}

static int run_server(const struct proxy_config *config)
{
    if (config->verbose)
    {
        printf("Verbose mode: ON\n");
    }

    write_log(LOG_INFO, "Server is listening to %s:%d", config->listen_ip, config->port);

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    get_worker_thread_attr();
    struct worker_context *workers = malloc(sizeof(struct worker_context) * config->threads);
    if(spawn_worker_fleet(workers,(int)config->threads,config->port,config->listen_ip,config->timeout)==1)
    {
        return 1;
    }

    while (server_running)
    {
        sleep(1);
    }
    destroy_worker_thread_attr();
    write_log(LOG_INFO, "Shutting down proxy server gracefully...");
    for (int i = 0; i < (int)config->threads; i++)
    {
        pthread_join(workers[i].thread, NULL);
    }
    free(workers);
    return 0;
}

int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (tls_init() != 0)
    {
        write_log(LOG_ERROR, "Failed to initialize TLS");
        close_logger();
        return 1;
    }
    long default_threads = sysconf(_SC_NPROCESSORS_ONLN);
    default_threads = (default_threads<0) ? 4 : default_threads;
    struct proxy_config config = {
        .port = 17777,
        .listen_ip = "127.0.0.1",
        .verbose = false,
        .mode = MODE_SERVER,
        .threads = default_threads,
        .timeout = 10,
        .max_log_files = 5,
        .max_log_size = 2*1024*1024,
        .log_dir = "logs"
    };
    FILE *config_file = fopen("proxy.conf","r");
    if(config_file!=NULL)
    {
        parse_config_file(config_file,&config);
        fclose(config_file);
    }
    if (init_logger(config.log_dir,config.max_log_files,config.max_log_size) == 1)
    {
        printf("Failed to initialise logger.\n");
    }

    if (parse_config(argc, argv, &config) == 1)
    {
        write_log(LOG_ERROR, "Invalid input");
        close_logger();
        return 1;
    }

    int status = 0;

    switch (config.mode)
    {
        case MODE_LOGS:
            status = get_logs();
            break;

        case MODE_HELP:
            printf("Usage: ./api -p <port> -b <ip> -t <threads> -m <server|stress|logs> -l <debug|info|warn|error> -v\n");
            break;

        case MODE_STRESS:
            printf("Stress test mode not implemented yet!\n");
            break;

        case MODE_SERVER:
        default:
            status = run_server(&config);
            break;
    }

    close_logger();
    tls_cleanup();
    return status;
}
