#ifndef CONFIG_H
#define CONFIG_H

struct proxy_config
{
    char *name;
    int port;
};

int parse_config(int argc, char **argv, struct proxy_config *config);

void free_config(struct proxy_config *config);

#endif
