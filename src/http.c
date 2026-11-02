#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "http.h"

static int parseRequestLine(char *line, struct http_request *req)
{
    if (line == NULL || req == NULL) return 1;

    char *method_end = strchr(line, ' ');
    if (method_end == NULL) return 1;

    *method_end = '\0';
    req->method = line;

    char *target_start = method_end + 1;
    char *target_end = strchr(target_start, ' ');
    if (target_end == NULL) return 1;

    *target_end = '\0';
    req->path = target_start;
    req->version = target_end + 1;

    return 0;
}

static char *trim(char *s)
{
    if (s == NULL || *s == '\0') return s;
    char *start = s;
    size_t len = strlen(start);
    if (len == 0) return start;
    char *end = start + len - 1;
    if (*start == ' ') start++;
    if (end >= start && (*end == ' ' || *end == '\r')) *end-- = '\0';

    if (*start == ' ' || *start == '\t')
    {
        while (*start == ' ' || *start == '\t') start++;
    }
    if (end >= start && (*end == ' ' || *end == '\t' || *end == '\r'))
    {
        while (end >= start && (*end == ' ' || *end == '\t' || *end == '\r'))
        {
            *end-- = '\0';
        }
    }
    return start;
}

static int add_header(char *line, struct http_header *h)
{
    if (line == NULL || h == NULL) return 1;
    char *colon = strchr(line, ':');
    if (colon == NULL) return 1;
    *colon = '\0';
    if (colon > line && (colon[-1]==' ' || colon[-1]=='\t'))
    {
        char *end = colon - 1;
        while (end >= line && (*end == ' ' || *end == '\t'))
        {
            *end-- = '\0';
        }
    }

    h->name = line;
    h->value = trim(colon + 1);
    return 0;
}


static int parse_headers(char *buffer,struct http_headers *headers)
{
    if(buffer==NULL || headers==NULL) return 1;
    if (headers->capacity == 0)
        headers->capacity = 8;
    headers->items = headers->inline_items;
    headers->capacity = HTTP_MAX_INLINE_HEADERS;
    headers->count = 0;
    char *rest=strstr(buffer,"\r\n");
    while(rest!=NULL)
    {
        if(rest==buffer) break;
        *rest='\0';
        if(headers->count==headers->capacity)
        {
            size_t new_capacity = headers->capacity * 2;
            struct http_header *new_item;
            if (headers->items == headers->inline_items) {
                new_item = malloc(new_capacity * sizeof(struct http_header));
                if (new_item != NULL) {
                    memcpy(new_item, headers->inline_items, headers->count * sizeof(struct http_header));
                }
            } else {
                new_item = realloc(headers->items, new_capacity * sizeof(struct http_header));
            }
            if (new_item == NULL) return 1;
            headers->items = new_item;
            headers->capacity = new_capacity;
        }
        if(add_header(buffer,&headers->items[headers->count])==1)
        {
            return 1;
        }
        headers->count++;
        buffer=rest+2;
        rest=strstr(buffer,"\r\n");
    }
    return 0;
}

void free_headers(struct http_headers *headers)
{
    if (headers == NULL) return;
    if (headers->items != NULL && headers->items != headers->inline_items)
    {
        free(headers->items);
    }
    headers->items = NULL;
    headers->capacity = 0;
    headers->count = 0;
}

void free_request(struct http_request *req)
{
    free_headers(&req->headers);
    req->method=NULL;
    req->path=NULL;
    req->version=NULL;
    req->body=NULL;
    req->body_length=0;
}

int parse_request(char *buffer, struct http_request *req)
{
    if(buffer==NULL || req==NULL) return 1;
    char *line_end =strstr(buffer,"\r\n");
    if(line_end==NULL) return 1;
    *line_end='\0';

    if(parseRequestLine(buffer,req)==1)
    {
        printf("Invalid request.\n");
        return 1;
    }
    if(parse_headers(line_end+2,&req->headers)==1)
    {
        printf("Invalid request.\n");
        free_request(req);
        return 1;
    }
    return 0;
}

const char *get_header_value(const struct http_headers *headers, const char *name)
{
    if (name == NULL || headers == NULL || headers->items == NULL) return NULL;
    char target_c = name[0];
    for (size_t i = 0; i < headers->count; i++)
    {
        if (headers->items[i].name != NULL && headers->items[i].value != NULL)
        {
            char h_c = headers->items[i].name[0];
            if ((h_c == target_c || (h_c ^ target_c) == 32) && strcasecmp(headers->items[i].name, name) == 0)
                return headers->items[i].value;
        }
    }
    return NULL;
}


static int parseResponseLine(char *line, struct http_response *res)
{
    if (line == NULL || res == NULL) return 1;

    char *version_end = strchr(line, ' ');
    if (version_end == NULL) return 1;

    *version_end = '\0';
    res->version = line;

    char *status_code_start = version_end + 1;
    char *status_code_end = strchr(status_code_start, ' ');
    if (status_code_end == NULL) return 1;

    *status_code_end = '\0';
    res->statusCode = (status_code_start[0] - '0') * 100 +
                  (status_code_start[1] - '0') * 10 +
                  (status_code_start[2] - '0');

    res->statusText = status_code_end + 1;

    return 0;
}

void free_response(struct http_response *res)
{
    free_headers(&res->headers);
    res->version=NULL;
    res->statusText=NULL;
    res->statusCode=0;
}

int parse_response(char *buffer, struct http_response *res)
{
    if(buffer==NULL || res==NULL) return 1;
    char *line_end =strstr(buffer,"\r\n");
    if(line_end==NULL) return 1;
    *line_end='\0';

    if(parseResponseLine(buffer,res)==1)
    {
        printf("Invalid response.\n");
        return 1;
    }
    if(parse_headers(line_end+2,&res->headers)==1)
    {
        printf("Invalid response.\n");
        free_response(res);
        return 1;
    }
    return 0;
}