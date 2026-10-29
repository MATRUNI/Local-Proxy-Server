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
    char listen_ip[64];
    bool verbose;
    enum proxy_mode mode;
    long threads;
    int timeout;
    size_t max_log_size;
    int max_log_files;
    char log_dir[128];
};

int parse_config(int argc, char **argv, struct proxy_config *config);

void free_config(struct proxy_config *config);

void parse_config_file(FILE *config_file,struct proxy_config *config);

#endif
