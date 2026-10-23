#ifndef HTTP_H
#define HTTP_H
#include <stddef.h>
struct http_header
{
    char *name;
    char *value;
};

struct http_headers
{
    struct http_header *items;
    size_t count;
    size_t capacity;
};

struct http_request
{
    char *method;
    char *path;
    char *version;
    struct http_headers headers;

    char *body;
    size_t body_length;
};

struct http_response {
    char *version;
    int statusCode;
    char *statusText;
    struct http_headers headers;
};

int parse_request(char *buffer,struct http_request *req);

void free_headers(struct http_headers *headers);

void free_request(struct http_request *req);

const char *get_header_value(const struct http_headers *headers, const char *name);

void free_response(struct http_response *res);

int parse_response(char *buffer, struct http_response *res);

#endif