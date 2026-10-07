#ifndef URL_H
#define URL_H

struct http_url
{
    char *schema;
    char *host;
    char *port;
    char *path;
};

int parse_url(char *input, struct http_url *url);

void free_url(struct http_url *url);

#endif