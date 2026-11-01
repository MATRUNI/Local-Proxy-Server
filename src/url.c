#include <string.h>
#include <stdlib.h>
#include "url.h"
#include <ctype.h>
#include <stdio.h>

int isNumber(char *str) {
    if (str == NULL || *str == '\0') return 1;
    int num = 0;
    for (int i = 0; str[i]!='\0'; i++) {
        if (str[i] < '0' || str[i] > '9') {
            return 1;
        }
        num = num*10 + (str[i]-'0');
    }
    if( 0 < num && num <= 65535)
    return 0;
    else return 1;
}

static void url_decode_in_place(char *str)
{
    char *src = str;
    char *dst = str;
    char a, b;

    while (*src)
    {
        if (*src == '%' &&
            (a = src[1]) && (b = src[2]) &&
            isxdigit((unsigned char)a) && isxdigit((unsigned char)b))
        {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';

            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';

            *dst++ = (char)(16 * a + b);
            src += 3;
        }
        else if (*src == '+')
        {
            *dst++ = ' ';
            src++;
        }
        else
        {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

void free_url(struct http_url *url)
{
    if(url==NULL) return;
    memset(url,0,sizeof(*url));
}

int validate_port(char *port)
{
    if (port == NULL || strcmp(port, "0") == 0)
    return 1;

    size_t len = strlen(port);
    if(len>5 || isNumber(port)==1) 
    {
        return 1;
    }
    return 0;
}

static int validate_host(char *host)
{
    if (host == NULL || *host == '\0') return 1;

    if (host[0] == '.' || host[0] == '-') return 1;

    for (int i = 0; host[i] != '\0'; i++)
    {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.'))
        return 1; 
        if (c == '.')
        {
            if (host[i + 1] == '\0' || host[i + 1] == '-' || host[i + 1] == '.')
            return 1;
        }
        if (c == '-' && (i == 0 || host[i - 1] == '.'))
        return 1; 

        if (c == '-' && (host[i + 1] == '\0' || host[i + 1] == '.'))
        return 1;
    }
    return 0;
}

int parse_url(char *input, struct http_url *url)
{
    int return_code = 1;
    if(input ==NULL || url==NULL) return 1;
    url_decode_in_place(input);
    char *host = strstr(input,"://");
    if(host==NULL) return return_code;

    size_t len = host - input;
    
    if (strncmp(input, "http", 4) == 0 || strncmp(input, "https", 5) == 0)
    {
        if (len >= sizeof(url->schema)) return return_code;
        strncpy(url->schema, input, len);
        url->schema[len] = '\0';
    }

    host+=3;
    if ( *host=='\0' || *host=='/')
    {
        goto cleanup;
    }

    char *port = strchr(host,':');
    char *path;
    if(port!=NULL)
    {
        // *port='\0';
        len = port - host;
        if(len == 0 || len >= sizeof(url->host)) goto cleanup;
        strncpy(url->host,host,len);
        url->host[len]='\0';
        port++;
        if(*port == '\0' || *port == '/')
        {
            goto cleanup;
        }
        path = strchr(port,'/');
        if(path==NULL)
        {
            strcpy(url->path,"/");

            if(validate_port(port)==1)
            {
                goto cleanup;
            }
            strcpy(url->path, "/");
            strcpy(url->port, port);
            return 0;
        }
        len = path - port;
        if(len == 0 || len>=sizeof(url->port) || len>5)
            goto cleanup;
        strncpy(url->port,port,len);
        url->port[len]='\0';
        if(validate_port(url->port)==1 || strlen(path) >= sizeof(url->path)) goto cleanup;
        strcpy(url->path,path);
    }
    else
    {
        strcpy(url->port, strcmp(url->schema, "https") == 0 ? "443" : "80");

        path = strchr(host,'/');
        if(path==NULL)
        {
            strcpy(url->path, "/");
            len = strlen(host);
            if(len >= sizeof(url->host) || validate_host(host)==1) goto cleanup;
            strcpy(url->host,host);
        }
        else
        {
            if (strlen(path) >= sizeof(url->path)) goto cleanup;
            strcpy(url->path,path);
            len = path - host;
        }
        
        if(path != NULL)
        {
            len = path - host;
            if(len == 0 || len >= sizeof(url->host)) goto cleanup;
            strncpy(url->host,host,len);
            url->host[len]='\0';
            if(validate_host(url->host)==1)
            {
                goto cleanup;
            }
        }
    }
    return 0;
    cleanup:
    free_url(url);
    return return_code;
}
