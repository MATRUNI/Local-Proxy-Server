#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>


#include "logger.h"
#include "config.h"
#include "url.h"
#include "net_compat.h"

static long get_online_cores(void)
{
    #if defined(_WIN32)
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        long online_core = sysinfo.dwNumberOfProcessors;
    #else
        long online_core = sysconf(_SC_NPROCESSORS_ONLN);
    #endif
    return online_core;
}

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
                    write_log(LOG_ERROR, "Invalid port.");
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
        else if (strcasecmp(argv[i], "-l") == 0 || strcasecmp(argv[i], "--log-level") == 0)
        {
            if (i + 1 < argc)
            {
                const char *val = argv[++i];
                if (strcasecmp(val, "debug") == 0 || strcasecmp(val, "d") == 0)
                {
                    change_log_level(LOG_DEBUG);
                }
                else if (strcasecmp(val, "info") == 0 || strcasecmp(val, "i") == 0)
                {
                    change_log_level(LOG_INFO);
                }
                else if (strcasecmp(val, "warn") == 0 || strcasecmp(val, "w") == 0)
                {
                    change_log_level(LOG_WARN);
                }
                else if (strcasecmp(val, "error") == 0 || strcasecmp(val, "e") == 0)
                {
                    change_log_level(LOG_ERROR);
                }
                else
                {
                    printf("Unknown log level: %s (expected: debug, info, warn, error)\n", val);
                    return 1;
                }
            }
        }
        else if (strcasecmp(argv[i], "-v") == 0 || strcasecmp(argv[i], "--verbose") == 0)
        {
            config->verbose = true;
            change_log_level(LOG_DEBUG);    
        }
        else if (strcasecmp(argv[i], "-b") == 0 || strcasecmp(argv[i], "--bind") == 0)
        {
            if (i + 1 < argc)
            {
                printf("Bind: %s\n", argv[++i]);
                strcpy(config->listen_ip, argv[i]);
            }
        }
        else if (strcasecmp(argv[i], "-t") == 0 || strcasecmp(argv[i], "--threads") == 0)
        {
            long online_core = get_online_cores();
            if(online_core == -1)
            {
                write_log(LOG_ERROR,"OS fail to detect Number of Processors currently Online.");
                return 1;
            }
            if (i + 1 < argc)
            {
                char *end;
                long value=strtol(argv[++i],&end,10);
                if(*end!='\0')
                {
                    write_log(LOG_ERROR, "Invalid thread count: '%s' is not a number", argv[i]);
                    return 1;
                }
                if(value < 1)
                {
                    write_log(LOG_ERROR,"Thread count must be at least 1 (got %ld). Using default (%d).",value,config->threads);
                    write_log(LOG_INFO,"Continuing with default thread count.");
                }
                else if(value>online_core*2)
                {
                    write_log(LOG_WARN,"%ld threads on %ld cores, seriously bro? Capping to %ld.",value,online_core,online_core*2);
                    config->threads = online_core*2;
                    write_log(LOG_INFO,"Continuing with max threads count.");
                }
                else
                {
                    config->threads = value;
                }
            }
            else
            {
                write_log(LOG_ERROR, "Option -t / --threads requires a numerical argument");
                return 1;
            }
            write_log(LOG_INFO,"Number of Processors currently Online: %ld",online_core);
            write_log(LOG_DEBUG,"Max number of threads allowed: %ld",online_core*2);
        }
        else if (strcasecmp(argv[i], "-h") == 0 || strcasecmp(argv[i], "--help") == 0)
        {
            config->mode = MODE_HELP;
        }
        else if (strcasecmp(argv[i], "--spawn-config") == 0)
        {
            const char *template =
                "# ==============================================================================\n"
                "# Local-Proxy-Server Configuration File\n"
                "# Auto-generated via --spawn-config\n"
                "# ==============================================================================\n\n"
                "# --- [Network Settings] ---\n"
                "port = 8080\n"
                "bind = 127.0.0.1\n"
                "timeout = 10\n\n"
                "# --- [Worker Thread Pool] ---\n"
                "# threads = 8\n\n"
                "# --- [Carousel Log Rotation] ---\n"
                "log_dir = logs\n"
                "max_log_size = 2MB\n"
                "max_log_files = 5\n";

            FILE *fp = fopen("proxy.conf", "w");
            if (fp == NULL)
            {
                perror("fopen");
                return 1;
            }

            fputs(template, fp);
            fclose(fp);

            printf("Successfully spawned default configuration file: proxy.conf\n");
            exit(0);
        }
        else
        {
            write_log(LOG_ERROR, "Invalid arguments.");
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

int validate_ip(const char *ip)
{   
    struct in_addr addr;
    return (inet_pton(AF_INET, ip, &addr) == 1) ? 0 : 1;
}

int parse_size(const char *str, size_t *bytes)
{
    char *end =NULL;
    unsigned long long val = strtoull(str,&end,10);
    if (str == end)
        return -1;

    uint64_t mul;
    if (*end == '\0' || strcasecmp(end, "B") == 0)
        mul = 1;
    else if (strcasecmp(end, "KB") == 0)
        mul = 1024ULL;
    else if (strcasecmp(end, "MB") == 0)
        mul = 1024ULL * 1024;
    else if (strcasecmp(end, "GB") == 0)
        mul = 1024ULL * 1024 * 1024;
    else
        return -1;
    if (val > UINT64_MAX / mul)
        return -1;
    *bytes = (size_t)val * mul;
    return 0;
    
}

void make_changes(char *key, char *val, struct proxy_config *config)
{
    if(strcasecmp(key,"port")==0)
    {
        if(validate_port(val)==0)
            config->port = atoi(val);
        return;
    }
    else if(strcasecmp(key,"bind")==0)
    {
        if(validate_ip(val)==0)
            strcpy(config->listen_ip,val);
        return;
    }
    else if(strcasecmp(key,"threads")==0)
    {
        if(isNumber(val)==0)
        {
            long online_core = get_online_cores();
            if(online_core == -1)
            {
                write_log(LOG_ERROR,"OS fail to detect Number of Processors currently Online.");
                return;
            }
            char *end;
            long value=strtol(val,&end,10);
            if(*end!='\0')
            {
                write_log(LOG_ERROR, "Invalid thread count: '%s' is not a number", val);
                return;
            }
            if(value < 1)
            {
                write_log(LOG_ERROR,"Thread count must be at least 1 (got %ld). Using default (%d).",value,config->threads);
                write_log(LOG_INFO,"Continuing with default thread count.");
            }
            else if(value>online_core*2)
            {
                write_log(LOG_WARN,"%ld threads on %ld cores, seriously bro? Capping to %ld.",value,online_core,online_core*2);
                config->threads = online_core*2;
                write_log(LOG_INFO,"Continuing with max threads count.");
            }
            else
            {
                config->threads = value;
            }
        }
        return;
    }
    else if(strcasecmp(key,"max_log_files")==0)
    {
        if(isNumber(val)==0)
        {
            int value = atoi(val);
            if(value>=2 && value<=50)
                config->max_log_files = (size_t) value;
        }
        return;
    }
    else if(strcasecmp(key,"max_log_size")==0)
    {
        parse_size((const char *)val,&config->max_log_size);
        return;
    }
    else if(strcasecmp(key,"log_dir")==0)
    {
        DIR *dir = opendir(val);
        if(dir!=NULL)
        {
            closedir(dir);
            strcpy(config->log_dir,val);
        }
        return;
    }
    else if(strcasecmp(key,"timeout")==0)
    {
        if(isNumber(val)==0)
        {
            int t = atoi(val);
            if (t > 0 && t <= 300)
            config->timeout = t;
        }
        return;
    }
    else if (strcasecmp(key, "log_level") == 0)
    {    
        if (strcasecmp(val, "debug") == 0) change_log_level(LOG_DEBUG);
        else if (strcasecmp(val, "info") == 0) change_log_level(LOG_INFO);
        else if (strcasecmp(val, "warn") == 0) change_log_level(LOG_WARN);
        else if (strcasecmp(val, "error") == 0) change_log_level(LOG_ERROR);
    }
}

void parse_config_file(FILE *config_file,struct proxy_config *config)
{
    char line[256];
    while (fgets(line,sizeof(line),config_file)!=NULL)
    {
        char *key=NULL, *value=NULL;
        bool got_key=false;
        for(int i=0;line[i]!='\0';i++)
        {
            if(line[i] == ' ')
            {
                line[i]='\0';
                continue;
            }
            if(line[i]=='#' || line[i]=='\n')
            {
                line[i]='\0';
                break;
            }
            if(line[i]=='=')
            {
                line[i]='\0';
                got_key=true;
            }
            if(!got_key && key==NULL && ((line[i]>='A' && line[i]<='Z') || (line[i]>='a' && line[i]<='z')))
            {
                key = &line[i];
            }
            else if(got_key && value==NULL && ((line[i]>='A' && line[i]<='Z') || (line[i]>='a' && line[i]<='z') || (line[i]>='0' && line[i]<='9')))
            {
                value = line + i;
            }
            else if(!((line[i]>='A' && line[i]<='Z') || (line[i]>='a' && line[i]<='z') || (line[i]>='0' && line[i]<='9') || line[i]=='_' || line[i]=='-' || line[i]=='.' || line[i]=='/'))
            {
                line[i]='\0';
            }
        }
        if(key!=NULL && value!=NULL)
        {
            make_changes(key,value,config);
        }
    }
}
