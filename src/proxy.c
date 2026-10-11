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
    while (s<total)
    {
        ssize_t sent = send(fd, buffer + s, total - s, 0);
        
        if (sent == -1)
        {
            perror("send");
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

int send_headers(int fd,const char *buffer, char *headers_end ,size_t total)
{
    struct http_headers headers={0};
    // 1. Find the end of the HTTP status line / request line (e.g., "HTTP/1.1 200 OK\r\n")
    const char *line_end = strstr(buffer,"\r\n");
    if(line_end==NULL) return 1;
    // 2. Parse all remaining headers starting right after the first line
    if(parse_headers((char *)line_end+2,&headers)==1)
    {
        free_headers(&headers);
        return 1;
    }

    // 3. Allocate a new buffer for the modified response. 
    // We add 64 bytes of extra safety padding on top of 'total' just in case sizes shift slightly.
    size_t new_capacity = total + 265;
    char *new_buffer = malloc(new_capacity);
    if(new_buffer==NULL) return 1;

    // 4. Copy the first line (Status/Request line) over to our new buffer
    size_t new_len = line_end - buffer + 2; //  +2 includes \r\n
    memcpy(new_buffer,buffer,new_len);

    char *cors_headers[] = {
        "Access-Control-Allow-Origin",
        "Access-Control-Allow-Methods",
        "Access-Control-Allow-Headers",
        "Access-Control-Expose-Headers"
    };

    // 5. Loop through all parsed headers, modify "Connection" to "close", and format them
    for (size_t i = 0; i < headers.count; i++)
    {
        if( headers.items[i].name !=NULL)
        {
            const char *value = headers.items[i].value;
            int temp;
            // Case-insensitive check for the "Connection" header
            if(strcasecmp(headers.items[i].name,"connection")==0)
            {
                value = "close";
            }
            if(strcasecmp(headers.items[i].name,"Access-Control-Allow-Origin")==0
              || strcasecmp(headers.items[i].name,"Access-Control-Allow-Methods")==0
              || strcasecmp(headers.items[i].name,"Access-Control-Allow-Headers")==0
              || strcasecmp(headers.items[i].name,"Access-Control-Expose-Headers")==0)
            {
                continue;
            }

            temp =write_header(new_buffer+new_len,new_capacity-new_len,headers.items[i].name,value);
            if(temp<0)
            {
                free_headers(&headers);
                free(new_buffer);
                return -1;
            }
            new_len+=temp;
        }
    }
    char *cors_values[] = {
        "*",
        "GET, POST, PUT, DELETE, OPTIONS",
        "*",
        "Content-Length, Content-Type",
    };
    for(int i=0;i<4;i++)
    {
        int temp=write_header(new_buffer+new_len,new_capacity-new_len,cors_headers[i],cors_values[i]);
        if(temp<0)
        {
            free_headers(&headers);
            free(new_buffer);
            return -1;
        }
        new_len+=temp;
    }
    // 6. Append the final \r\n to close out the header section (\r\n\r\n separator)
    memcpy(new_buffer+new_len,"\r\n",2);
    new_len+=2;
    // 7. Locate and copy the original response body data right after the headers
    const char *body = headers_end + 4;
    size_t body_len = total - (body - buffer);
    
    memcpy(new_buffer + new_len, body, body_len);
    size_t final_total_len = new_len + body_len;
    if(send_all(fd,new_buffer,final_total_len)==1)
    {
        free(new_buffer);
        free_headers(&headers);
        return 1;
    }
    // 9. Clean up allocated memory to avoid memory leaks
    free(new_buffer);
    free_headers(&headers);
    return 0; // success
}

int sendHTTPRequest(int server_fd, struct http_request *req ,struct http_url *url)
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

int receiveHTTPResponse(int server_fd, int client_fd)
{
    char *buffer = malloc(4096);
    if(buffer==NULL) return 1;
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
        int n = recv(server_fd,buffer+total, capacity-total-1,0);
        if(n==-1)
        {
            perror("recv");
            free(buffer);
            close(server_fd);
            return 1;
        }
        if (n == 0) break;
        total+=n;
        buffer[total]='\0';
        headers_end = strstr(buffer,"\r\n\r\n");
    }
    if(headers_end==NULL)
    {
        fprintf(stderr, "Incomplete HTTP response headers.\n");
        free(buffer);
        close(server_fd);
        return 1;
    }
    printf("Response buffer: \n");
    size_t w=0;
    while(w<total)
    {
        if (buffer[w] == '\r')
            printf("\\r");
        else if (buffer[w] == '\n')
            printf("\\n\n");
        else
            printf("%c", buffer[w]);
        w++;
    }
    long len = find_capacity(buffer);
    if(len==-1)
    {
        free(buffer);
        close(server_fd);
        return 1;
    }
    if(send_headers(client_fd,buffer,headers_end,total)==1)
    {
        free(buffer);
        close(server_fd);
        return 1;
    }
    headers_end+=4;
    long remaining_body = len - ((buffer + total) - headers_end);
    if(remaining_body<0)
    {
        free(buffer);
        close(server_fd);
        return 1;    
    }
    while (remaining_body>0)
    {
        int n;
        if(remaining_body>=(long)capacity)
        n = recv(server_fd,buffer, capacity,0);
        else
        n = recv(server_fd,buffer, remaining_body,0);

        if(n==-1)
        {
            perror("recv");
            close(server_fd);
            free(buffer);
            return 1;
        }
        remaining_body-=n;
        if (n == 0 && remaining_body>0) break;

        if(send_all(client_fd,buffer,n)==1)
        {
            close(server_fd);
            free(buffer);
            return 1;
        }
    }
    if(remaining_body>0)
    {
        free(buffer);
        close(server_fd);
        return 1;
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

    if(strcasecmp(req.method,"OPTIONS")==0)
    {
        if(handle_options_request(client_fd)==1)
        {
            free_request(&req);
            free(buffer);
            return 1;
        }
        free_request(&req);
        free(buffer);
        return 0;
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
