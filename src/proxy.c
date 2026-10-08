#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <string.h>
#include <ctype.h>

#include "url.h"
#include "http.h"
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
            printf("Connected\n");
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

    while (1)
    {
        ssize_t sent = send(fd, buffer + s, total - s, 0);

        if (sent == -1)
        {
            perror("send");
            return 1;
        }
        s += sent;
        if (total == s)
            break;
        if (sent == 0)
            return 1;
    }
    return 0;
}

int sendHTTPRequest(int server_fd, struct http_request *req ,struct http_url *url)
{
    char request[MAX_HEADER_SIZE];
    int offset = 0;

    offset += snprintf(request + offset,sizeof(request) - offset,
        "%s %s %s\r\n",req->method,url->path,req->version);

    offset += snprintf(request + offset,sizeof(request) - offset,
        "Host: %s\r\n",url->host);

    for (size_t i = 0; i < req->headers.count; i++)
    {
        offset += snprintf(request + offset,sizeof(request) - offset,
            "%s: %s\r\n",
            req->headers.items[i].name,req->headers.items[i].value
        );
    }

    offset += snprintf(request + offset,sizeof(request) - offset,"\r\n");

    if(send_all(server_fd,request,offset)==1)
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
    if(chunk_end - buffer <=total) return 1;
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

int receiveHTTPResponse(int server_fd, int client_fd)
{
    size_t capacity = BUFFER_SIZE;
    char *buffer = malloc(capacity);
    if(buffer==NULL) return 1;

    while(1)
    {
        ssize_t n = recv(server_fd,buffer, capacity,0);
        if(n==-1)
        {
            perror("recv");
            free(buffer);
            close(server_fd);
            return 1;
        }
        if (n == 0) break;

        if(send_all(client_fd,buffer,n)==1)
        {
            free(buffer);
            close(server_fd);
            return 1;
        }
    }
    free(buffer);
    close(server_fd);
    return 0;
}

int proxy_handle_client(int client_fd)
{
    if(client_fd<=0) return 1;
    char *buffer = malloc(4096);
    if(buffer==NULL) return 1;
    size_t total=0;
    size_t capacity=4096;
    buffer[0]='\0';
    while(strstr(buffer,"\r\n\r\n")==NULL)
    {
        if(total >=capacity-1)
        {
            capacity*=2;
            if(capacity>MAX_HEADER_SIZE)
            {
                printf("ERROR: Header too large.\n");
                free(buffer);
                return 1;
            }
            char * new_buf = realloc(buffer,capacity);
            if(new_buf==NULL)
            {
                free(buffer);
                return 1;
            }
            buffer=new_buf;
        }
        int n = recv(client_fd,buffer+total,capacity-total-1,0);
        if (n == -1)
        {
            perror("recv");
            free(buffer);
            close(client_fd);
            return 1;
        }
        if (n == 0)
        {
            printf("Client disconnected.\n");
            break;
        }

        total+=n;
        buffer[total]='\0';
        if(total >=capacity-1)
        {
            printf("Buffer full.'\n");
            break;
        }
    }
    struct http_request req={0};
    if(parse_request(buffer,&req)==1)
    {
        free(buffer);
        printf("Invalid request.\n");
        return 1;
    }
    printf("%s %s %s\n",req.method,req.path,req.version);
    struct http_url url={0};
    if(parse_url(req.path,&url)==1)
    {
        free_request(&req);
        free(buffer);
        printf("Invalid URL.\n");
        return 1;
    }

    int server_fd = make_api_call(url.host,url.port);
    if(server_fd==-1)
    {
        free_request(&req);
        free_url(&url);
        free(buffer);
        return 1;
    }
    
    if(sendHTTPRequest(server_fd,&req,&url)==1)
    {
        free_request(&req);
        free_url(&url);
        free(buffer);
        close(server_fd);
        return 1;
    }
    
    if(receiveHTTPResponse(server_fd,client_fd)==1)
    {
        free_request(&req);
        free_url(&url);
        free(buffer);
        close(server_fd);
        return 1;
    }

    free_request(&req);
    free_url(&url);
    free(buffer);
    close(server_fd);
    return 0;
}
