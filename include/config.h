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

int parse_config(int argc, char **argv, struct proxy_config *config);

void free_config(struct proxy_config *config);

#endif
