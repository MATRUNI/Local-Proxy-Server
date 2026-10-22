#ifndef CONFIG_H
#define CONFIG_H
#include <stdbool.h>
#define THREAD_STACK_SIZE (256 * 1024)
#define LOGGER_THREAD_STACK_SIZE (32 * 1024)
#define PROXY_TIMEOUT_SEC 10

enum proxy_mode
{
    MODE_SERVER,
    MODE_STRESS,
    MODE_LOGS,
    MODE_HELP
};
struct proxy_config
{
    int port;
    const char *listen_ip;
    bool verbose;
    enum proxy_mode mode;
    long threads;
};

typedef enum {
    /* -------------------------------------------------------------
     * INGRESS PHASES (Client -> Proxy)
     * ------------------------------------------------------------- */
    LEVEL_CLIENT_READ_HEADERS, // Reading "GET / HTTP/1.1\r\n..."
    LEVEL_UPSTREAM_SEND_REQ, // Transmitting HTTP request to target
    
    /* -------------------------------------------------------------
    * EGRESS PHASES (Proxy -> Upstream Target)
    * ------------------------------------------------------------- */
    LEVEL_CLIENT_READ_BODY, // Reading incoming POST/PUT payloads
    LEVEL_UPSTREAM_CONNECT, // Resolving DNS & TCP/TLS handshake
    LEVEL_UPSTREAM_READ_RESP, // Ingesting response headers & body from target

    /* -------------------------------------------------------------
     * TRANSMISSION & COMPLETION (Proxy -> Client)
     * ------------------------------------------------------------- */
    LEVEL_CLIENT_WRITE_RESP, // Flushing response bytes to client (Backpressure)
    LEVEL_COMPLETE             // All data transferred; clean teardown
} session_level_t;


int parse_config(int argc, char **argv, struct proxy_config *config);

void free_config(struct proxy_config *config);

#endif
