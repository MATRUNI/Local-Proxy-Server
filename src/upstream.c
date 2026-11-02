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


static int send_headers(int fd,char *new_buffer,size_t total, struct http_headers *headers, size_t remaining ,int cors_included)
{
    for (size_t i = 0; i < headers->count; i++)
    {
        if (strncasecmp(headers->items[i].name, "Access-Control-", 15) == 0)
            continue;
        if( headers->items[i].name !=NULL)
        {
            const char *value = headers->items[i].value;
            int temp;
            temp =write_header(new_buffer+remaining,total-remaining,headers->items[i].name,value);
            if(temp<0)
            {
                return -1;
            }
            remaining+=temp;
        }
    }

    if (cors_included == 0)
    {
        static const char cors_block[] =
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Access-Control-Expose-Headers: Content-Length, Content-Type\r\n";
        size_t cors_len = sizeof(cors_block) - 1;
        if (remaining + cors_len >= total)
        {
            return -1;
        }
        memcpy(new_buffer + remaining, cors_block, cors_len);
        remaining += cors_len;
    }

    memcpy(new_buffer + remaining, "\r\n", 2);
    remaining += 2;
    if(send_all(fd,new_buffer,remaining)==1)
    {
        return 1;
    }
    return 0; // success
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
    while(remaining_bytes>0)
    {
        size_t to_read = remaining_bytes > (long) capacity ? capacity : (size_t) remaining_bytes;
        int n = to_client ? con->read(con, buffer, to_read) : recv(con->client_fd, buffer, to_read, 0);
        if(n<=0)
        {
            if(n==0) break;
            if (socket_errno == EAGAIN_COMPAT || socket_errno == EWOULDBLOCK_COMPAT) {
                write_log(LOG_WARN, "%s read timed out after 10s.", to_client ? "Upstream" : "Client");
            } else {
                perror("recv");
            }
            return 1;
        }
        int err = to_client ? send_all(con->client_fd, buffer, n) : conn_write_all(con, buffer, n);
        if (err == 1) return 1;
        remaining_bytes -=n;
    }
    return 0;
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


int receiveHTTPResponse(struct proxy_connection *con)
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
    
    struct http_response res={0};
    if(parse_response(buffer,&res)!=0)
    {
        goto cleanup_buffer;
    }

    size_t header_out_cap = total + 1024;
    char stack_out[4096];
    char *header_out_buf = (header_out_cap <= sizeof(stack_out)) ? stack_out : malloc(header_out_cap);
    if (header_out_buf == NULL)
    {
        free_response(&res);
        goto cleanup_buffer;
    }

    size_t vlen = strlen(res.version);
    size_t tlen = strlen(res.statusText);
    if (vlen + tlen + 7 >= header_out_cap)
    {
        if (header_out_buf != stack_out)
            free(header_out_buf);
        free_response(&res);
        goto cleanup_buffer;
    }
    memcpy(header_out_buf, res.version, vlen);
    header_out_buf[vlen] = ' ';
    header_out_buf[vlen + 1] = '0' + (res.statusCode / 100);
    header_out_buf[vlen + 2] = '0' + ((res.statusCode / 10) % 10);
    header_out_buf[vlen + 3] = '0' + (res.statusCode % 10);
    header_out_buf[vlen + 4] = ' ';
    memcpy(header_out_buf + vlen + 5, res.statusText, tlen);
    memcpy(header_out_buf + vlen + 5 + tlen, "\r\n", 2);
    size_t headers_len = vlen + 5 + tlen + 2;


    if (send_headers(con->client_fd, header_out_buf, header_out_cap, &res.headers, headers_len, 0) == 1)
    {
        if(header_out_buf!=stack_out)
            free(header_out_buf);
        free_response(&res);
        goto cleanup_buffer;
    }
    if(header_out_buf!=stack_out)
        free(header_out_buf);
    const char *len_str = get_header_value(&res.headers, "Content-Length");
    con->status_code = res.statusCode;
    if (len_str != NULL)
    {
        long content_length = strtol(len_str, NULL, 10);
        con->body_bytes_sent = (content_length>0) ? (size_t)content_length : 0;
        free_response(&res);
        if(stream_body(con,true,buffer,capacity,total,headers_end,content_length)==1)
        {
            goto cleanup_buffer;
        }
    }
    else
    {
        free_response(&res);
        char *body_start = headers_end+4;
        size_t initial_body_bytes = total - (body_start-buffer);
        con->body_bytes_sent=initial_body_bytes;
        if(initial_body_bytes>0)
        {
            if(send_all(con->client_fd,body_start,initial_body_bytes)==1)
            {
                goto cleanup_buffer;
            }
        }
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
