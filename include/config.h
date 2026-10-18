#ifndef CONFIG_H
#define CONFIG_H
#include <stdbool.h>
#define THREAD_STACK_SIZE (256 * 1024)

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
};

int parse_config(int argc, char **argv, struct proxy_config *config);

void free_config(struct proxy_config *config);

#endif
