#ifndef UPSTREAM_H
#define UPSTREAM_H

#include "proxy.h"
#include "url.h"
#include "http.h"
#include <stdbool.h>

int make_api_call(const char *host, const char *port, bool *in_progress);

int stream_body(struct proxy_connection *con,bool to_client,char *buffer,size_t capacity, size_t total,char *headers_end,long content);

int sendHTTPRequest(struct proxy_connection *con, struct http_request *req ,struct http_url *url);

int receiveHTTPResponse(struct proxy_connection *con);

#endif
