#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "logger.h"
#include "config.h"

int parse_config(int argc, char **argv, struct proxy_config *config)
{
    for(int i=1;i<argc;i++)
    {
        if (strcasecmp(argv[i], "-p") == 0)
        {
            if (i + 1 < argc)
            {
                char *end;
                long value=strtol(argv[++i],&end,10);
                if(*end!='\0' || value<1 || value>65535)
                {
                    write_log("Invalid port.");
                    close_logger();
                    return 1;
                }
                config->port = (int)value;
            }
        }
        else if (strcasecmp(argv[i], "-m") == 0 || strcasecmp(argv[i], "--mode") == 0)
        {
            if (i + 1 < argc)
            {
                printf("Mode: %s\n", argv[++i]);
                if (strcasecmp(argv[i], "logs") == 0)
                {
                    config->mode = MODE_LOGS;
                }
                else if (strcasecmp(argv[i], "stress") == 0)
                {
                    config->mode = MODE_STRESS;
                }
                else if (strcasecmp(argv[i], "-h") == 0 || strcasecmp(argv[i], "--help") == 0)
                {
                    config->mode = MODE_HELP;
                }
                else
                {
                    config->mode = MODE_SERVER;
                }
            }
        }
        else if (strcasecmp(argv[i], "-b") == 0 || strcasecmp(argv[i], "--bind") == 0)
        {
            if (i + 1 < argc)
            {
                printf("Bind: %s\n", argv[++i]);
                config->listen_ip = argv[i];
            }
        }
        else if (strcasecmp(argv[i], "-v") == 0 || strcasecmp(argv[i], "--verbose") == 0)
        {
            config->verbose = true;
        }
        else
        {
            write_log("Invalid arguments.");
            close_logger();
            return 1;
        }    
    }
    return 0;
}

void free_config(struct proxy_config *config)
{
    (void)config;
}