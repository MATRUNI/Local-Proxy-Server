#include <openssl/ssl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#include "proxy.h"
#include "tls.h"
#include "logger.h"

static SSL_CTX *global_ssl_ctx = NULL;

int tls_init(void)
{
    if(global_ssl_ctx!=NULL) return 0;

    global_ssl_ctx = SSL_CTX_new(TLS_client_method());

    if(global_ssl_ctx==NULL) return 1;

    SSL_CTX_set_default_verify_paths(global_ssl_ctx);
    return 0;
}

void tls_cleanup(void)
{
    if(global_ssl_ctx!=NULL)
    {
        SSL_CTX_free(global_ssl_ctx);
        global_ssl_ctx=NULL;
    }
}

ssize_t ssl_write_wrapper(struct proxy_connection *conn, void *buf, size_t len)
{
    return SSL_write((SSL *)conn->upstream_ssl,buf,len);
}

ssize_t plain_write_wrapper(struct proxy_connection *conn, void *buf, size_t len)
{
    return send(conn->upstream_fd,buf,len,0);
}

ssize_t ssl_read_wrapper(struct proxy_connection *conn, void *buf, size_t len)
{
    return SSL_read((SSL *)conn->upstream_ssl,buf,len);
}

ssize_t plain_read_wrapper(struct proxy_connection *conn, void *buf, size_t len)
{
    return recv(conn->upstream_fd,buf,len,0);
}

int tls_connect(struct proxy_connection *conn, const char *hostname,bool is_https)
{
    if(is_https)
    {
        SSL *ssl = SSL_new(global_ssl_ctx);
        if(ssl==NULL) return 1;
        SSL_set_fd(ssl,conn->upstream_fd);
        SSL_set_tlsext_host_name(ssl,hostname);
        if(SSL_connect(ssl)<=0)
        {
            SSL_free(ssl);
            return 1;
        }
        conn->upstream_ssl = ssl;
        conn->read = ssl_read_wrapper;
        conn->write = ssl_write_wrapper;
        return 0;
    }
    else
    {
        conn->read = plain_read_wrapper;
        conn->write = plain_write_wrapper;
        return 0;
    }
}

int conn_write_all(struct proxy_connection *conn, const void *buf, size_t total)
{
    size_t sent = 0;
    while (sent < total) {
        ssize_t n = conn->write(conn, (char *)buf + sent, total - sent);
        if (n == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                write_log(LOG_WARN, "Upstream server write timed out after 10s.");
            else if (errno == EPIPE || errno == ECONNRESET)
                write_log(LOG_WARN, "Upstream server closed connection unexpectedly.");
            else
                perror("send");
            return 1;
        }
        sent += n;
    }
    return 0;
}

void conn_close(struct proxy_connection *conn)
{
    if(conn==NULL) return;
    if(conn->upstream_ssl!=NULL)
    {
        SSL_shutdown((SSL *)conn->upstream_ssl);
        SSL_free((SSL *)conn->upstream_ssl);
        conn->upstream_ssl=NULL;
    }
    if(conn->upstream_fd>=0)
    {
        close(conn->upstream_fd);
        conn->upstream_fd=-1;
    }
}