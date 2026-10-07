
#include <string.h>
#include <stdlib.h>
#include "url.h"

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

void free_url(struct http_url *url)
{
    if(url==NULL) return;
    free(url->schema);
    free(url->host);
    free(url->port);
    free(url->path);
    url->host=NULL;
    url->schema=NULL;
    url->port=NULL;
    url->path=NULL;
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

int validate_host(char *host)
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
    if(input ==NULL || url==NULL) return 1;
    char *host = strstr(input,"://");
    if(host==NULL) return 1;

    size_t len = host - input;
    
    if(strncmp(input,"http",4)==0 || strncmp(input,"https",5)==0)
    {
        url->schema=malloc(len+1);
        if(url->schema!=NULL)
        {
            strncpy(url->schema,input,len);
            url->schema[len]='\0';
        }
        else return 1;
    }
    else
    {
        return 1;
    }

    host+=3;
    if ( *host=='\0' || *host=='/')
    {
        free_url(url);
        return 1;
    }

    char *port = strchr(host,':');
    char *path;
    if(port!=NULL)
    {
        // *port='\0';
        len = port - host;
        url->host = malloc(len+1);
        if(url->host==NULL || len==0)
        {
            free_url(url);
            return 1;
        }
        else
        {
            strncpy(url->host,host,len);
            url->host[len]='\0';
        }
        port++;
        if(*port == '\0' || *port == '/')
        {
            free_url(url);
            return 1;
        }
        path = strchr(port,'/');
        if(path==NULL)
        {
            url->path=malloc(2);
            if(url->path==NULL)
            {
                free_url(url);
                return 1;
            }
            strcpy(url->path,"/");

            if(validate_port(port)==1)
            {
                free_url(url);
                return 1;
            }
            len = strlen(port);
            url->port = malloc(len+1);
            if(url->port==NULL)
            {
                free_url(url);
                return 1;
            }
            strcpy(url->port,port);
            url->port[len]='\0';
            return 0;
        }
        len = path - port;
        if(len>5)
        {
            free_url(url);
            return 1;
        }
        char temp[6];
        strncpy(temp,port,len);
        temp[len]='\0';
        if(validate_port(temp)==1) 
        {
            free_url(url);
            return 1;
        }
        url->port=malloc(strlen(temp)+1);
        if(url->port==NULL)
        {
            free_url(url);
            return 1;
        }
        strcpy(url->port,temp);
        url->path = malloc(strlen(path) + 1);

        if (url->path == NULL)
        {
            free_url(url);
            return 1;
        }

        strcpy(url->path,path);
    }
    else
    {
        url->port = malloc(4);

        if (url->port == NULL)
        {
            free_url(url);
            return 1;
        }
        strcpy(url->port, strcmp(url->schema, "https") == 0 ? "443" : "80");

        path = strchr(host,'/');
        if(path==NULL)
        {
            url->path = malloc(2);

            if (url->path == NULL)
            {
                free_url(url);
                return 1;
            }
            strcpy(url->path, "/");
            len = strlen(host);
            if(validate_host(host)==1)
            {
                free_url(url);
                return 1;
            }
        }
        else
        {
            len = strlen(path);
            url->path = malloc(len + 1);
            if (url->path == NULL)
            {
                free_url(url);
                return 1;
            }
            strcpy(url->path,path);
            len = path - host;
        }
        url->host= malloc(len+1);
        if(url->host==NULL)
        {
            free_url(url);
            return 1;
        }
        if(path==NULL)
        strcpy(url->host,host);
        else
        {
            strncpy(url->host,host,len);
            url->host[len]='\0';
            if(validate_host(url->host)==1)
            {
                free_url(url);
                return 1;
            }
        }
    }
    return 0;
}
