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
    pthread_t thread;
};

#define MAX_EVENTS 64


void handle_sigint(int sig)
{
    (void)sig;
    server_running = 0;
}

void *proxy_thead_worker(void *arg)
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
            int client_fd = events[i].data.fd;
            set_socket_timeout(client_fd);
            proxy_handle_client(client_fd);
            close(client_fd);
        }
    }
    close(self->epoll_fd);
    return NULL;    
}

void get_worker_thread_attr()
{
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);
}

void destroy_worker_thread_attr(void)
{
    pthread_attr_destroy(&attr);
}

int spawn_worker_fleet(struct worker_context *workers, int threads)
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
        if (pthread_create(&workers[i].thread, &attr, proxy_thead_worker, &workers[i]) != 0)
        {
            write_log(LOG_ERROR, "Out of threads.");
            return 1;
        }
    }
    return 0;
}

int run_server(const struct proxy_config *config)
{
    if (config->verbose)
    {
        printf("Verbose mode: ON\n");
    }

    struct proxy_server server = {0};
    if (server_init(&server, config->port, config->listen_ip) == 1)
    {
        return 1;
    }

    write_log(LOG_INFO, "Server is listening to %s:%d", config->listen_ip, config->port);

    struct sigaction sa = {0};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    get_worker_thread_attr();
    struct worker_context *workers = malloc(sizeof(struct worker_context) * config->threads);
    if(spawn_worker_fleet(workers,(int)config->threads)==1)
    {
        return 1;
    }

    while (server_running)
    {
        int client_fd = server_accept(&server);
        if (client_fd == -1)
        {
            continue;
        }
        int target_worker = client_fd % config->threads;
        struct epoll_event ev = {
            .data.fd = client_fd,
            .events = EPOLLIN | EPOLLET
        };
        epoll_ctl(workers[target_worker].epoll_fd,EPOLL_CTL_ADD,client_fd,&ev);
    }
    destroy_worker_thread_attr();
    write_log(LOG_INFO, "Shutting down proxy server gracefully...");
    for (int i = 0; i < (int)config->threads; i++)
    {
        pthread_join(workers[i].thread, NULL);
    }
    free(workers);
    server_destroy(&server);
    return 0;
}

int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (init_logger() == 1)
    {
        printf("Failed to initialise logger.\n");
    }

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
        .threads = default_threads
    };

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
