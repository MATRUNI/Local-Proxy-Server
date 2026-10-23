#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>

#include "url.h"
#include "http.h"
#include "logger.h"
#include "proxy.h"
#include "tls.h"
#include "http_error.h"
#include "server.h"
#include "upstream.h"

#define MAX_HEADER_SIZE (65536)

struct client_session *session_create(int client_fd,int epoll_fd)
{
    struct client_session *session = malloc(sizeof(struct client_session));
    if (session == NULL) return NULL;
    memset(&session->con, 0, sizeof(session->con));
    session->con.client_fd = client_fd;
    session->con.upstream_fd = -1;
    session->epoll_fd = epoll_fd;
    session->level = LEVEL_CLIENT_READ_HEADERS;
    session->capacity = 4096;
    session->total = 0;
    session->offset = 0;
    session->buffer = malloc(session->capacity);
    session->url= (struct http_url){0};
    session->req = (struct http_request){0};

    if (session->buffer == NULL)
    {
        free(session);
        return NULL;
    }
    session->buffer[0] = '\0';
    return session;
}

void session_free(struct client_session *session)
{
    if (session == NULL) return;
    if (session->buffer != NULL)
    {
        free(session->buffer);
        session->buffer = NULL;
    }
    free_url(&session->url);
    free_request(&session->req);
    conn_close(&session->con);
    if (session->con.client_fd >= 0)
    {
        close(session->con.client_fd);
        session->con.client_fd = -1;
    }
    free(session);
}

static int handle_options_request(int client_fd)
{
    const char *preflight_response = 
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n";
    
    return send_all(client_fd, preflight_response, strlen(preflight_response));
}

static void send_client_error(int client_fd, int status_code, const char *message)
{
    char err[512];
    int len = format_error_response(err, sizeof(err), status_code, message);
    if (len > 0)
    {
        send_all(client_fd, err, len);
    }
}

int proxy_handle_client(struct client_session *session)
{
    int res = PROXY_ERROR;
    if(session==NULL) return res;
    int client_fd = session->con.client_fd;
    char *buffer = session->buffer;
    size_t total= session->total;
    size_t capacity= session->capacity;

    char *headers_end = (session->offset > 0) ? (buffer + session->offset) : NULL;

    switch (session->level)
    {
        case LEVEL_CLIENT_READ_HEADERS:
            goto LEVEL_CLIENT_READ_HEADERS;
        case LEVEL_UPSTREAM_CONNECT:
            goto LEVEL_UPSTREAM_CONNECT;
        
        default:
            break;
    }
    LEVEL_CLIENT_READ_HEADERS:
    while(headers_end==NULL)
    {
        if(total >=capacity-1)
        {
            capacity*=2;
            if(capacity>MAX_HEADER_SIZE)
            {
                write_log(LOG_WARN, "Header too large.");
                send_client_error(client_fd, 413, "Request headers exceed maximum allowed size\n");
                return res;
            }
            char * new_buf = realloc(buffer,capacity);
            if(new_buf==NULL)
            {
                send_client_error(client_fd, 500, "Internal server error: out of memory\n");
                return res;
            }
            buffer=new_buf;
        }
        int n = recv(client_fd,buffer+total,capacity-total-1,0);
        if (n == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                write_log(LOG_WARN, "Waiting for more data from client.");
                session->total = total;
                session->capacity = capacity;
                session->buffer = buffer;
                return PROXY_AGAIN;
            } else {
                perror("recv");
            }
            return res;
        }
        if (n == 0)
        {
            return res;
        }

        total+=n;
        buffer[total]='\0';
        headers_end = strstr(buffer,"\r\n\r\n");
    }
    session->level = LEVEL_UPSTREAM_CONNECT;
    session->total = total;
    session->buffer = buffer;
    session->capacity = capacity;
    if(headers_end==NULL)
    {
        send_client_error(client_fd, 400, "Incomplete HTTP request headers\n");
        return res;
    }
    session->offset = (size_t)(headers_end - buffer);
    if(parse_request(buffer,&session->req)==1)
    {
        send_client_error(client_fd, 400, "Malformed HTTP request syntax\n");
        return res;
    }

    if(strcasecmp(session->req.method,"OPTIONS")==0)
    {
        if(handle_options_request(client_fd)==1)
        {
            goto cleanup_req;
        }
        res = 0;
        goto cleanup_req;
    }
    const char *methods[] = {"GET","POST","PUT","DELETE","HEAD","PATCH"};
    for(size_t i=0;i<6;i++)
    {
        if(strcmp(session->req.method,methods[i])==0)
        {
            break;
        }
        if(i==5)
        {
            write_log(LOG_WARN, "Invalid request method.");
            send_client_error(client_fd, 405, "HTTP method not supported by proxy\n");
            goto cleanup_req;
        }
    }

    write_log(LOG_INFO, "%s %s %s",session->req.method,session->req.path,session->req.version);
    const char *target = get_header_value(&session->req.headers,"X-Target-Url");
    if(target==NULL && strncmp(session->req.path,"/?url=",6)==0)
    {
        target = session->req.path+6;
    }
    if(target==NULL && (strncmp(session->req.path,"https://",8)==0 || strncmp(session->req.path,"http://",7)==0))
    {
        target = session->req.path;
    }

    if(target==NULL || parse_url((char *)target,&session->url)==1)
    {
        write_log(LOG_WARN, "Invalid or missing target URL.");
        send_client_error(client_fd, 400, "Invalid or missing target URL format\n");
        goto cleanup_req;
    }
    bool in_progress = false;
    int server_fd = make_api_call(session->url.host,session->url.port, &in_progress);
    if(server_fd==-1)
    {
        send_client_error(client_fd, 502, "Failed to connect to target server (DNS or connection refused)\n");
        goto cleanup_url;
    }
    session->con.upstream_fd = server_fd;
    session->con.upstream_ssl = NULL;

    struct epoll_event ev = {
        .data.ptr = session,
        .events = EPOLLOUT | EPOLLET | EPOLLONESHOT
    };
    epoll_ctl(session->epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);
    session->level = LEVEL_UPSTREAM_CONNECT;
    if(in_progress)
    {
        return PROXY_AGAIN;
    }
    
    LEVEL_UPSTREAM_CONNECT:
    headers_end = buffer + session->offset;

    if(check_socket_connected(session->con.upstream_fd)!=0)
    {
        send_client_error(client_fd, 502, "TLS/SSL handshake with target server failed\n");
        goto cleanup_con;
    }
    epoll_ctl(session->epoll_fd, EPOLL_CTL_DEL, session->con.upstream_fd, NULL);
    set_socket_blocking(session->con.upstream_fd);
    set_socket_timeout(session->con.upstream_fd);
    bool is_https = (strcasecmp(session->url.schema,"https")==0);
    
    if(tls_connect(&session->con,session->url.host,is_https)==1)
    {
        send_client_error(client_fd, 502, "TLS/SSL handshake with target server failed\n");
        goto cleanup_con;
    }

    if(sendHTTPRequest(&session->con,&session->req,&session->url)==1)
    {
        send_client_error(client_fd, 502, "Failed to send request headers to target server\n");
        goto cleanup_con;
    }

    const char *len_str = get_header_value(&session->req.headers, "Content-Length");
    if (len_str != NULL)
    {
        long content_length = strtol(len_str, NULL, 10);
        if(content_length>0)
        {
            set_socket_blocking(session->con.client_fd);
            set_socket_timeout(session->con.client_fd);
            if(stream_body(&session->con,false,buffer,capacity,total,headers_end,content_length)==1)
            {
                send_client_error(client_fd, 502, "Failed to stream request body to target server\n");
                goto cleanup_con;
            }
        }
    }
    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    if(receiveHTTPResponse(&session->con)==1)
    {
        send_client_error(client_fd, 502, "Target server closed connection without valid HTTP response\n");
        goto cleanup_con;
    }

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    long latency_ms = (end_ts.tv_sec - start_ts.tv_sec) * 1000 + (end_ts.tv_nsec - start_ts.tv_nsec) / 1000000;
    write_log(LOG_INFO, "%s %s -> %d (%zu bytes) in %ld ms",session->req.method, session->url.host, session->con.status_code, session->con.body_bytes_sent ,latency_ms);

    res = PROXY_DONE;
    cleanup_con:
    conn_close(&session->con);
    cleanup_url:
    cleanup_req:
    return res;
}
