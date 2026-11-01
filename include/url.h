#ifndef URL_H
#define URL_H

struct http_url
{
    char schema[8];
    char host[128];
    char port[8];
    char path[1024];
};

int parse_url(char *input, struct http_url *url);

int validate_port(char *port);

int isNumber(char *str);

void free_url(struct http_url *url);

#endif