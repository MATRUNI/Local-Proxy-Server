// include/tls.h
#ifndef TLS_H
#define TLS_H

#include <stdbool.h>
#include <stddef.h>

struct proxy_connection;
int tls_init(void);

void tls_cleanup(void);

int tls_connect(struct proxy_connection *conn, const char *hostname, bool is_https);

void conn_close(struct proxy_connection *conn);

int conn_write_all(struct proxy_connection *conn, const void *buf, size_t total);

#endif
