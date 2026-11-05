#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "upstream.h"
#include "logger.h"
#include "tls.h"
#include "server.h"
#include "net_compat.h"

#define MAX_HEADER_SIZE (65536)

static int connectToDNS(struct addrinfo *adr,bool *in_progress)
{
    for(struct addrinfo *p =adr;p!=NULL;p=p->ai_next)
    {    
        int server_fd = socket(p->ai_family,p->ai_socktype,p->ai_protocol);
        if(server_fd==-1)
        {
            continue;
        }
        set_socket_non_blocking(server_fd);
        set_socket_nodelay(server_fd);
        int res = connect(server_fd,p->ai_addr,p->ai_addrlen);
        if(res==0)
        {
            *in_progress = false;
            return server_fd;
        }
        if (res==-1 && socket_errno == EINPROGRESS_COMPAT)
        {
            *in_progress = true;
            return server_fd;
        }
        close_socket(server_fd);
    }
    return -1;
}

int make_api_call(const char *host, const char *port, bool *in_progress)
{
    if(host==NULL || port==NULL) return -1;
    struct sockaddr_in sin = {0};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(atoi(port));
    if (inet_pton(AF_INET, host, &sin.sin_addr) == 1)
    {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd != -1)
        {
            set_socket_non_blocking(server_fd);
            set_socket_nodelay(server_fd);
            int res = connect(server_fd, (struct sockaddr *)&sin, sizeof(sin));
            if (res == 0)
            {
                *in_progress = false;
                return server_fd;
            }
            if (res == -1 && socket_errno == EINPROGRESS_COMPAT)
            {
                *in_progress = true;
                return server_fd;
            }
            close_socket(server_fd);
        }
    }
    struct addrinfo hints={0};
    struct addrinfo *adr={0};

    memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_INET;
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_protocol=IPPROTO_TCP;

    int status=getaddrinfo(host,port,&hints,&adr);
    if(status!=0) return -1;

    int server_fd=connectToDNS(adr,in_progress);
    if(server_fd==-1)
    {
        freeaddrinfo(adr);
        return -1;
    }

    freeaddrinfo(adr);
    return server_fd;
}

static int write_header(char *buffer, size_t len, const char *name, const char *value)
{
    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    if (nlen + vlen + 4 >= len) return -1;

    memcpy(buffer, name, nlen);
    buffer += nlen;
    memcpy(buffer, ": ", 2);
    buffer += 2;
    memcpy(buffer, value, vlen);
    buffer += vlen;
    memcpy(buffer, "\r\n", 2);
    return (int)(nlen + vlen + 4);
}

static int stream_remaining(struct proxy_connection *con, bool to_client, char *buffer, size_t capacity, long remaining_bytes)
{
    while (remaining_bytes > 0)
    {
        size_t to_read = remaining_bytes > (long)capacity ? capacity : (size_t)remaining_bytes;
        int n = to_client ? con->read(con, buffer, to_read) : recv(con->client_fd, buffer, to_read, 0);
        if (n <= 0)
        {
            if (n == 0) break;
            if (socket_errno == EAGAIN_COMPAT || socket_errno == EWOULDBLOCK_COMPAT) {
                write_log(LOG_WARN, "%s read timed out after 10s.", to_client ? "Upstream" : "Client");
            } else {
                perror("recv");
            }
            return 1;
        }
        int err = to_client ? send_all(con->client_fd, buffer, n) : conn_write_all(con, buffer, n);
        if (err == 1) return 1;
        remaining_bytes -= n;
    }
    return 0;
}

int stream_body(struct proxy_connection *con,bool to_client,char *buffer,size_t capacity, size_t total,char *headers_end,long content)
{
    if(content<=0 || buffer==NULL || headers_end==NULL) return 1;
    char *body_start = headers_end+4;
    size_t header_len = (size_t)(body_start - buffer);
    size_t initial_body_bytes = (total > header_len) ? (total - header_len) : 0;

    if (initial_body_bytes > 0)
    {
        int err = to_client 
            ? send_all(con->client_fd, body_start, initial_body_bytes) 
            : conn_write_all(con, body_start, initial_body_bytes);
        if (err == 1) return 1;
    }
    long remaining_bytes = content - initial_body_bytes;
    
    return (remaining_bytes > 0) ? stream_remaining(con, to_client, buffer, capacity, remaining_bytes) : 0;
}

int sendHTTPRequest(struct proxy_connection *con, struct http_request *req, struct http_url *url)
{
    char request[8192];
    size_t offset = 0;

    size_t mlen = strlen(req->method);
    size_t plen = strlen(url->path);
    size_t vlen = strlen(req->version);
    if (offset + mlen + plen + vlen + 4 >= sizeof(request)) return 1;

    memcpy(request + offset, req->method, mlen);
    offset += mlen;
    request[offset++] = ' ';
    memcpy(request + offset, url->path, plen);
    offset += plen;
    request[offset++] = ' ';
    memcpy(request + offset, req->version, vlen);
    offset += vlen;
    memcpy(request + offset, "\r\n", 2);
    offset += 2;

    int temp = write_header(request + offset, sizeof(request) - offset, "Host", url->host);
    if (temp < 0) return 1;
    offset += temp;

    for (size_t i = 0; i < req->headers.count; i++)
    {
        char h0 = req->headers.items[i].name[0];
        if ((h0 == 'H' || h0 == 'h') && strcasecmp(req->headers.items[i].name, "Host") == 0)
        {
            continue;
        }
        temp = write_header(request + offset, sizeof(request) - offset, req->headers.items[i].name, req->headers.items[i].value);
        if (temp < 0) return 1;
        offset += temp;
    }

    if (offset + 2 >= sizeof(request)) return 1;
    memcpy(request + offset, "\r\n", 2);
    offset += 2;

    return conn_write_all(con, request, offset);
}

int receiveHTTPResponse(struct proxy_connection *con, bool client_close)
{
    int return_code = 1;
    char stack_buf[4096];
    char *buffer = stack_buf;
    size_t total=0;
    size_t capacity=sizeof(stack_buf);

    char *headers_end = NULL;

    while(headers_end==NULL)
    {
        if(total >=capacity-1)
        {
            capacity*=2;
            if(capacity>MAX_HEADER_SIZE)
            {
                write_log(LOG_WARN, "Header too large.");
                goto cleanup_buffer;
            }
            char *new_buf;
            if (buffer == stack_buf) {
                new_buf = malloc(capacity);
                if (new_buf != NULL) {
                    memcpy(new_buf, stack_buf, total + 1);
                }
            } else {
                new_buf = realloc(buffer, capacity);
            }
            if(new_buf==NULL)
            {
                goto cleanup_buffer;
            }
            buffer=new_buf;
        }
        int n = con->read(con,buffer+total, capacity-total-1);
        if (n == -1)
        {
            if (socket_errno == EAGAIN_COMPAT || socket_errno == EWOULDBLOCK_COMPAT) {
                write_log(LOG_WARN, "Upstream server response timed out after 10s.");
            }
            goto cleanup_buffer;
        }
        if (n == 0) break;
        total+=n;
        buffer[total]='\0';
        headers_end = strstr(buffer,"\r\n\r\n");
    }
    if(headers_end==NULL)
    {
        fprintf(stderr, "Incomplete HTTP response headers.\n");
        goto cleanup_buffer;
    }
    int status_code = 200;
    char *sp = strchr(buffer, ' ');
    if (sp != NULL && sp < headers_end) {
        status_code = (int)strtol(sp + 1, NULL, 10);
    }
    con->status_code = status_code;
    
    long content_length = -1;
    char *cl = strcasestr(buffer, "\ncontent-length:");

    if (cl != NULL && cl < headers_end)
        content_length = strtol(cl + 16, NULL, 10);
    size_t headers_len = (size_t)(headers_end + 4 - buffer);
    size_t initial_body = (total > headers_len) ? (total - headers_len) : 0;

    if(content_length >= 0 && initial_body > (size_t)content_length)
        initial_body = (size_t)content_length;

    if (client_close) {
        char *conn_hdr = strcasestr(buffer, "\nconnection:");
        if (conn_hdr != NULL && conn_hdr < headers_end) {
            char *ka = strcasestr(conn_hdr, "keep-alive");
            if (ka != NULL && ka < headers_end) {
                memcpy(ka, "close     ", 10);
            }
        }
    }

    size_t to_send = headers_len + initial_body;
    if (send_all(con->client_fd, buffer, to_send) == 1)
        goto cleanup_buffer;

    if (content_length > 0)
    {
        con->body_bytes_sent = (size_t)content_length;
        long remaining = content_length - (long)initial_body;
        if( remaining>0 && stream_remaining(con,true,buffer,capacity,remaining)==1)
        {
            goto cleanup_buffer;
        }
    }
    else if(content_length == 0)
        con->body_bytes_sent = 0;
    else
    {
        char *body_start = headers_end+4;
        size_t initial_body_bytes = total - (body_start-buffer);
        con->body_bytes_sent=initial_body_bytes;

        if(initial_body_bytes >= 5  && memcmp(body_start + initial_body_bytes - 5, "0\r\n\r\n",5) == 0)
        {
            return_code = 0;
            goto cleanup_buffer;
        }
        while(1)
        {
            int n = con->read(con,buffer,capacity);
            if(n<=0) break;
            con->body_bytes_sent+=n;
            if(send_all(con->client_fd,buffer,n)==1)
            {
                goto cleanup_buffer;
            }
            if (n >= 5 && memcmp(buffer + n - 5, "0\r\n\r\n", 5) == 0) {
                break;
            }
        }
    }
    return_code = 0;
    cleanup_buffer:
    if(buffer != stack_buf)
        free(buffer);
    return return_code;
}
