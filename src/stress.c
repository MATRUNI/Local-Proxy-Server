#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include "stress.h"
#include "url.h"
#include "poll_engine.h"
#include "net_compat.h"
#include "server.h"
#include "thread_compat.h"
#include "proxy.h"
#include "tls.h"


enum stress_conn_state {
    CONN_CONNECTING,
    CONN_SENDING,
    CONN_RECEIVING
};

struct stress_connection {
    struct proxy_connection con;
    enum stress_conn_state state;
    size_t bytes_sent;
    char read_buf[2048];
    size_t read_bytes;
    struct stress_worker *worker;
};

struct stress_worker {
    int id;
    pthread_t thread;
    struct poll_engine *engine;
    struct stress_connection *conns;
    int conn_count;           // e.g. 25 connections
    long reqs_target;         // Target requests for this worker
    long reqs_completed;      // How many requests finished
    long reqs_failed;         // How many requests errored
    const char *request_buf;  // The assembled HTTP request to blast
    size_t request_len;
    struct sockaddr_in target_addr;
    struct http_url url;
};

int stress_engine_init(struct stress_connection *s)
{
    s->con.upstream_fd = socket(AF_INET,SOCK_STREAM,0);
    if(s->con.upstream_fd<0) return -1;
    set_socket_non_blocking(s->con.upstream_fd);
    set_socket_nodelay(s->con.upstream_fd);
    s->bytes_sent = 0;
    s->read_bytes = 0;
    int res = connect(s->con.upstream_fd,(struct sockaddr *)&s->worker->target_addr,sizeof(s->worker->target_addr));
    if(res==0)
        s->state = CONN_SENDING;
    if(res == 0 || (res==-1 && socket_errno == EINPROGRESS_COMPAT))
        s->state = CONN_CONNECTING;
    else
    {
        close_socket(s->con.upstream_fd);
        s->con.upstream_fd = -1;
        return -1;
    }
    s->worker->engine->add(s->worker->engine,s->con.upstream_fd,ENGINE_WRITE | ENGINE_ONESHOT,s);
    return 0;
}

int stress_conn_handle(struct stress_connection *s)
{
    if(s->state == CONN_CONNECTING)
    {
        if(check_socket_connected(s->con.upstream_fd)==0)
        {
            tls_connect(&s->con, s->worker->url.host, strcmp(s->worker->url.schema, "https") == 0);
            s->state = CONN_SENDING;
        }
        else
        {
            s->worker->reqs_failed++;
            conn_close(&s->con);
            return 1;
        }

    }
    while(s->bytes_sent < s->worker->request_len)
    {
        int n = s->con.write(&s->con,(char *)s->worker->request_buf+s->bytes_sent,s->worker->request_len-s->bytes_sent);
        if(n==0) break;
        if(n==-1)
        {
            if(socket_errno == EAGAIN_COMPAT || socket_errno == EWOULDBLOCK_COMPAT)
            {
                s->worker->engine->mod(s->worker->engine, s->con.upstream_fd, ENGINE_WRITE | ENGINE_ONESHOT, s);
            }
            else
            {
                perror("send");
                conn_close(&s->con);
            }
            return 1;
        }
        s->bytes_sent += n;
        if(s->bytes_sent == s->worker->request_len)
        {
            s->state = CONN_RECEIVING;
            s->read_bytes = 0;
            s->worker->engine->mod(s->worker->engine, s->con.upstream_fd, ENGINE_READ | ENGINE_ONESHOT, s);
            break;
        }
    }
    while(s->state == CONN_RECEIVING)
    {
        int n = s->con.read(&s->con,s->read_buf + s->read_bytes, sizeof(s->read_buf) - 1 - s->read_bytes);
        if(n == -1)
        {
            if(socket_errno == EAGAIN_COMPAT || socket_errno == EWOULDBLOCK_COMPAT)
                s->worker->engine->mod(s->worker->engine, s->con.upstream_fd, ENGINE_READ | ENGINE_ONESHOT, s);
            return 1;
        }
        if(n==0)
        {
            if(s->worker->reqs_completed < s->worker->reqs_target)
            {
                conn_close(&s->con);
                stress_engine_init(s);
                return 0;
            }
        }
        if(n>0)
        {
            s->read_bytes += n;
            s->read_buf[s->read_bytes]='\0';
            if (strstr(s->read_buf, "\r\n\r\n") != NULL)
            {
                s->worker->reqs_completed++;
                if(s->worker->reqs_completed < s->worker->reqs_target)
                {
                    s->bytes_sent = 0;
                    s->read_bytes = 0;
                    s->state = CONN_SENDING;
                    s->worker->engine->mod(s->worker->engine, s->con.upstream_fd, ENGINE_WRITE | ENGINE_ONESHOT, s);
                    break;
                }
                s->worker->engine->del(s->worker->engine, s->con.upstream_fd);
                conn_close(&s->con);
                break;
            }
        }
    }
    return 0;
}

static void *stress_worker_thread(void *arg)
{
    struct stress_worker *w = (struct stress_worker *)arg;
    w->engine = poll_engine_create();
    if(w->engine==NULL) return NULL;
    for(int i=0;i<w->conn_count;i++)
    {
        w->conns[i].worker=w;
        stress_engine_init(&w->conns[i]);
    }
    struct socket_info events[ENGINE_MAX_EVENTS];
    while((w->reqs_completed + w->reqs_failed) < w->reqs_target)
    {
        int n = w->engine->wait(w->engine, events, 500);
        if(n == -1) continue;
        for(int i = 0; i < n; i++)
        {
            struct stress_connection *s = events[i].user_data;
            if(s != NULL)
            {
                stress_conn_handle(s);
            }
        }
    }
    for (int i = 0; i < w->conn_count; i++) {
        conn_close(&w->conns[i].con);
    }

    w->engine->destroy(w->engine);
    return NULL;
}

int run_stress_engine(struct proxy_config *config)
{
    if (config->threads <= 0) config->threads = 4;
    if (config->stress.concurrency <= 0) config->stress.concurrency = 50;
    if (config->stress.total_requests <= 0) config->stress.total_requests = 10000;
    if (config->stress.target_url[0] == '\0') strcpy(config->stress.target_url, "http://127.0.0.1:17777/");

    struct http_url url;
    if (parse_url(config->stress.target_url, &url) != 0) {
        fprintf(stderr, "Failed to parse target URL: %s\n", config->stress.target_url);
        return 1;
    }
    struct sockaddr_in target_addr = {0};
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(atoi(url.port));

    if (inet_pton(AF_INET, url.host, &target_addr.sin_addr) != 1)
    {
        struct hostent *he = gethostbyname(url.host);
        if (he == NULL || he->h_addr_list[0] == NULL) {
            fprintf(stderr, "Failed to resolve host: %s\n", url.host);
            return 1;
        }
        memcpy(&target_addr.sin_addr, he->h_addr_list[0], sizeof(target_addr.sin_addr));
    }
    char req_buf[16384];
    int req_len;

    const char *path = url.path[0] != '\0' ? url.path : "/";
    const char *method = config->stress.method[0] != '\0' ? config->stress.method : "GET";

    if (config->stress.body_len > 0)
    {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: ProxyStress/1.0\r\n"
            "Accept: */*\r\n"
            "Content-Length: %zu\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "%s",
            method, path, url.host, config->stress.body_len, config->stress.body);
    }
    else
    {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: ProxyStress/1.0\r\n"
            "Accept: */*\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            method, path, url.host);
    }
    
    struct stress_worker *workers = calloc(config->threads,sizeof(struct stress_worker));
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if(workers==NULL) return 1;
    for(int i=0;i<(int)config->threads;i++)
    {
        workers[i].id = i;
        workers[i].conn_count = config->stress.concurrency/config->threads;
        if (workers[i].conn_count < 1) workers[i].conn_count = 1;
        workers[i].reqs_target = config->stress.total_requests/config->threads;
        workers[i].request_buf = req_buf;
        workers[i].request_len = req_len;
        workers[i].conns = calloc(workers[i].conn_count, sizeof(struct stress_connection));
        workers[i].url = url;
        workers[i].target_addr = target_addr;
        if(pthread_create(&workers[i].thread, NULL, stress_worker_thread,&workers[i])!=0)
        {
            return 1;
        }
    }
    long total_completed = 0, total_failed = 0;
    for(int i=0;i<(int)config->threads;i++)
    {
        pthread_join(workers[i].thread, NULL);
        total_completed += workers[i].reqs_completed;
        total_failed += workers[i].reqs_failed;
        free(workers[i].conns);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    free(workers);
    float duration = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    float rps = total_completed/duration;
    printf("\n=======================================================\n");
    printf("                  STRESS TEST RESULTS                  \n");
    printf("=======================================================\n");
    printf("  Target:             %s\n", config->stress.target_url);
    printf("  Method:             %s\n", config->stress.method[0] ? config->stress.method : "GET");
    printf("  Concurrency:        %d connections\n", config->stress.concurrency);
    printf("  Threads:            %ld worker threads\n", config->threads);
    printf("-------------------------------------------------------\n");
    printf("  Total Requests:     %ld\n", config->stress.total_requests);
    printf("  Completed:          %ld\n", total_completed);
    printf("  Failed:             %ld\n", total_failed);
    printf("  Duration:           %.3f seconds\n", duration);
    printf("-------------------------------------------------------\n");
    printf("  Throughput:         \033[1;32m%.2f req/sec\033[0m\n", rps);
    printf("=======================================================\n\n");
    return 0;
}
