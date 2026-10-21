#ifndef UPSTREAM_H
#define UPSTREAM_H

#include "proxy.h"
#include "url.h"
#include "http.h"

int connectToDNS(struct addrinfo *adr);

int make_api_call(const char *host, const char *port);

int write_header(char *buffer,size_t len,const char *name,const char *value);

int send_all(int fd,const char *buffer, size_t total);

int send_headers(int fd,char *new_buffer,size_t total, struct http_headers *headers, size_t remaining ,int cors_included);

int sendHTTPRequest(struct proxy_connection *con, struct http_request *req, struct http_url *url);

int stream_body(struct proxy_connection *con,bool to_client,char *buffer,size_t capacity, size_t total,char *headers_end,long content);

int sendHTTPRequest(struct proxy_connection *con, struct http_request *req ,struct http_url *url);

int receiveHTTPResponse(struct proxy_connection *con);

#endif
