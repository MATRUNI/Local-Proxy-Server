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

#include "url.h"
#include "http.h"
#include "logger.h"
#include "proxy.h"
#include "tls.h"
#include "http_error.h"
#include "server.h"

#define BUFFER_SIZE     (65536)
#define MAX_HEADER_SIZE (65536)

int connectToDNS(struct addrinfo *adr)
{
    for(struct addrinfo *p =adr;p!=NULL;p=p->ai_next)
    {    
        int server_fd = socket(p->ai_family,p->ai_socktype,p->ai_protocol);
        if(server_fd==-1)
        {
            continue;
        }
        if (connect(server_fd,p->ai_addr,p->ai_addrlen) == -1)
        {
            close(server_fd);
        }
        else
        {
            return server_fd;
        }
    }
    return -1;
}

int make_api_call(const char *host, const char *port)
{
    if(host==NULL || port==NULL) return -1;

    struct addrinfo hints={0};
    struct addrinfo *adr={0};

    memset(&hints,0,sizeof(hints));
    hints.ai_family=AF_INET;
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_protocol=IPPROTO_TCP;

    int status=getaddrinfo(host,port,&hints,&adr);
    if(status!=0) return -1;

    int server_fd=connectToDNS(adr);
    if(server_fd==-1)
    {
        freeaddrinfo(adr);
        return -1;
    }

    freeaddrinfo(adr);
    return server_fd;
}

int send_all(int fd,const char *buffer, size_t total)
{
    size_t s = 0;
    while (s<total)
    {
        ssize_t sent = send(fd, buffer + s, total - s, 0);
        
        if (sent == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                write_log(LOG_WARN, "Client write timed out (client not consuming data).");
            }
            else if (errno == EPIPE || errno == ECONNRESET) {
                write_log(LOG_WARN, "Client disconnected prematurely (broken pipe).");
            }
            else {
                perror("send");
            }
            return 1;
        }
        s += sent;
    }
    return 0;
}

int handle_options_request(int client_fd)
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

int write_header(char *buffer,size_t len,const char *name,const char *value)
{
    int written = snprintf(buffer,len,"%s: %s\r\n",name,value);
    if(written<0 || (size_t)written >= len)
    return -1;

    return written;
}

int send_headers(int fd,char *new_buffer,size_t total, struct http_headers *headers, size_t remaining ,int cors_included)
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

    if(cors_included==0)
    {
        char *cors_headers[] = {
            "Access-Control-Allow-Origin",
            "Access-Control-Allow-Methods",
            "Access-Control-Allow-Headers",
            "Access-Control-Expose-Headers"
        };
        char *cors_values[] = {
            "*",
            "GET, POST, PUT, DELETE, OPTIONS",
            "*",
            "Content-Length, Content-Type",
        };
        for(int i=0;i<4;i++)
        {
            int temp=write_header(new_buffer+remaining,total-remaining,cors_headers[i],cors_values[i]);
            if(temp<0)
            {
                return -1;
            }
            remaining+=temp;
        }
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
    size_t initial_body_bytes = total - (body_start-buffer);
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
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

int sendHTTPRequest(struct proxy_connection *con, struct http_request *req ,struct http_url *url)
{
    char request[MAX_HEADER_SIZE];
    size_t offset = 0;
    int temp;

    temp = snprintf(request + offset,sizeof(request) - offset,
        "%s %s %s\r\n",req->method,url->path,req->version);
    if(temp<0 || (size_t)temp >= sizeof(request)-offset) return 1;
    else
    {
        offset+=temp;
    }
    temp = snprintf(request + offset,sizeof(request) - offset,
    "Host: %s\r\n",url->host);
    if(temp<0 || (size_t)temp >= sizeof(request)-offset) return 1;
    else
    {
        offset+=temp;
    }

    for (size_t i = 0; i < req->headers.count; i++)
    {
        if (strcasecmp(req->headers.items[i].name, "Host") == 0)
        {
            continue;
        }
        temp = write_header(request+offset,sizeof(request)-offset,req->headers.items[i].name,req->headers.items[i].value);
        if(temp<0 || (size_t)temp >= sizeof(request)-offset) return 1;
        offset+=temp;
    }

    temp = snprintf(request + offset,sizeof(request) - offset,"\r\n");
    if(temp<0 || (size_t)temp >= sizeof(request)-offset) return 1;
    else
    {
        offset+=temp;
    }
    if(conn_write_all(con,request,offset)==1)
    {
        return 1;
    }

    return 0;
}

char *strstr_case_insensitive(const char *haystack, const char *needle)
{
    if (*needle == '\0') return (char *)haystack;
    for (; *haystack != '\0'; haystack++)
    {
        const char *h = haystack;
        const char *n = needle;
        while (*h != '\0' && *n != '\0' && tolower((unsigned char)*h) == tolower((unsigned char)*n))
        {
            h++;
            n++;
        }
        if (*n == '\0') return (char *)haystack;
    }
    return NULL;
}

long parse_chunk_size(char *buffer, char **end)
{
    if(buffer==NULL) return -1;
    char *p;
    long n = strtol(buffer,&p,16);
    if(*p!='\r') return -1;
    *end=p;
    return n;
}

int chunk_available(char *buffer, size_t total,char *size_end, long chunk_size)
{
    if(buffer==NULL || size_end==NULL) return 0;
    char *body = size_end+2;
    char *chunk_end = body + chunk_size;
    if((size_t)(chunk_end - buffer) <=total) return 1;
    return 0;
}

long find_capacity(char *buffer)
{
    char *cl = strstr_case_insensitive(buffer,"\r\nContent-Length");
    if(cl==NULL) return -1;
    cl+=2;
    char *colon = strchr(cl,':');
    if(colon==NULL) return -1;
    char *end;
    colon++;
    long len = strtol(colon,&end,10);
    if(end==colon) return -1;
    while (*end==' ')
    {
        end++;
    }
    if(*end!='\r') return -1;
    return len;
}

int find_encoding(char *buffer,char **start)
{
    char *cl = strstr_case_insensitive(buffer,"\r\nTransfer-Encoding");
    if(cl==NULL) return -1;
    cl+=2;
    char *colon = strchr(cl,':');
    if(colon==NULL) return -1;
    colon++;
    while (*colon==' ')
    {
        colon++;
    }
    *start = colon;
    return 0;
}

int receiveHTTPResponse(struct proxy_connection *con)
{
    int return_code = 1;
    char *buffer = malloc(4096);
    if(buffer==NULL) return return_code;
    size_t total=0;
    size_t capacity=4096;

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
            char * new_buf = realloc(buffer,capacity);
            if(new_buf==NULL)
            {
                goto cleanup_buffer;
            }
            buffer=new_buf;
        }
        int n = con->read(con,buffer+total, capacity-total-1);
        if (n == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
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
    char *header_out_buf = malloc(header_out_cap);
    if (header_out_buf == NULL)
    {
        free_response(&res);
        goto cleanup_buffer;
    }

    size_t headers_len = snprintf(header_out_buf, header_out_cap, "%s %d %s\r\n",
                                  res.version, res.statusCode, res.statusText);

    if (send_headers(con->client_fd, header_out_buf, header_out_cap, &res.headers, headers_len, 0) == 1)
    {
        free(header_out_buf);
        free_response(&res);
        goto cleanup_buffer;
    }

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
    free(buffer);
    return return_code;
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

int proxy_handle_client(int client_fd)
{
    int res = 1;
    if(client_fd<=0) return res;
    char *buffer = malloc(4096);
    if(buffer==NULL) return res;
    size_t total=0;
    size_t capacity=4096;
    buffer[0]='\0';
    char *headers_end=NULL;
    while(headers_end==NULL)
    {
        if(total >=capacity-1)
        {
            capacity*=2;
            if(capacity>MAX_HEADER_SIZE)
            {
                write_log(LOG_WARN, "Header too large.");
                send_client_error(client_fd, 413, "Request headers exceed maximum allowed size\n");
                goto cleanup_buffer;
            }
            char * new_buf = realloc(buffer,capacity);
            if(new_buf==NULL)
            {
                send_client_error(client_fd, 500, "Internal server error: out of memory\n");
                goto cleanup_buffer;
            }
            buffer=new_buf;
        }
        int n = recv(client_fd,buffer+total,capacity-total-1,0);
        if (n == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                write_log(LOG_WARN, "Client read timed out after 10s.");
            } else {
                perror("recv");
            }
            goto cleanup_buffer;
        }
        if (n == 0)
        {
            goto cleanup_buffer;
        }

        total+=n;
        buffer[total]='\0';
        headers_end = strstr(buffer,"\r\n\r\n");
    }
    if(headers_end==NULL)
    {
        send_client_error(client_fd, 400, "Incomplete HTTP request headers\n");
        goto cleanup_buffer;
    }
    struct http_request req={0};
    if(parse_request(buffer,&req)==1)
    {
        send_client_error(client_fd, 400, "Malformed HTTP request syntax\n");
        goto cleanup_buffer;
    }

    if(strcasecmp(req.method,"OPTIONS")==0)
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
        if(strcmp(req.method,methods[i])==0)
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

    write_log(LOG_INFO, "%s %s %s",req.method,req.path,req.version);
    const char *target = get_header_value(&req.headers,"X-Target-Url");
    if(target==NULL && strncmp(req.path,"/?url=",6)==0)
    {
        target = req.path+6;
    }
    if(target==NULL && (strncmp(req.path,"https://",8)==0 || strncmp(req.path,"http://",7)==0))
    {
        target = req.path;
    }

    struct http_url url={0};
    if(target==NULL || parse_url((char *)target,&url)==1)
    {
        write_log(LOG_WARN, "Invalid or missing target URL.");
        send_client_error(client_fd, 400, "Invalid or missing target URL format\n");
        goto cleanup_req;
    }
    int server_fd = make_api_call(url.host,url.port);
    if(server_fd==-1)
    {
        send_client_error(client_fd, 502, "Failed to connect to target server (DNS or connection refused)\n");
        goto cleanup_url;
    }
    set_socket_timeout(server_fd);
    bool is_https = (strcasecmp(url.schema,"https")==0);
    struct proxy_connection con = {
        .client_fd = client_fd,
        .upstream_fd = server_fd,
        .upstream_ssl = NULL
    };
    if(tls_connect(&con,url.host,is_https)==1)
    {
        send_client_error(client_fd, 502, "TLS/SSL handshake with target server failed\n");
        goto cleanup_con;
    }
    if(sendHTTPRequest(&con,&req,&url)==1)
    {
        send_client_error(client_fd, 502, "Failed to send request headers to target server\n");
        goto cleanup_con;
    }

    const char *len_str = get_header_value(&req.headers, "Content-Length");
    if (len_str != NULL)
    {
        long content_length = strtol(len_str, NULL, 10);
        if(content_length>0)
            if(stream_body(&con,false,buffer,capacity,total,headers_end,content_length)==1)
            {
                send_client_error(client_fd, 502, "Failed to stream request body to target server\n");
                goto cleanup_con;
            }   
    }
    struct timespec start_ts, end_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    if(receiveHTTPResponse(&con)==1)
    {
        send_client_error(client_fd, 502, "Target server closed connection without valid HTTP response\n");
        goto cleanup_con;
    }

    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    long latency_ms = (end_ts.tv_sec - start_ts.tv_sec) * 1000 + (end_ts.tv_nsec - start_ts.tv_nsec) / 1000000;
    write_log(LOG_INFO, "%s %s -> %d (%zu bytes) in %ld ms",req.method, target, con.status_code, con.body_bytes_sent ,latency_ms);

    res = 0;
    cleanup_con:
    conn_close(&con);
    cleanup_url:
    free_url(&url);
    cleanup_req:
    free_request(&req);
    cleanup_buffer:
    free(buffer);
    return res;
}
