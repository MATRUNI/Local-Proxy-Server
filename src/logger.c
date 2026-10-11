#include <stdio.h>
#include "logger.h"
#include <stdarg.h>

static FILE * logger = NULL;

int init_logger()
{
    if(logger!=NULL)
    {
        return 0;
    }
    logger = fopen("logs/logger.log","a+");
    if(logger==NULL)
    {
        perror("fopen");
        printf("Failed to open file.\n");
        return 1;
    }
    return 0;
}

int write_log(const char *line,...)
{
    if(logger==NULL || line==NULL) 
    {
        printf("Logger isn't availalbe.\n");
        return 1;
    }
    va_list args;
    va_start(args,line);
    int res = vfprintf(logger,line,args);
    va_end(args);
    if(res<0)
    {
        perror("vfprintf");
        return 1;
    }
    res = fputc('\n',logger);
    if(res<0)
    {
        perror("fputc");
        return 1;
    }
    int flush_response = fflush(logger);
    if(flush_response==0)
    return 0;
    return 1;
}

int get_logs()
{
    if(logger==NULL) return 1;
    rewind(logger);
    char line[300];
    while (fgets(line,sizeof(line),logger)!=NULL)
    {
        printf("%s",line);
    }
    if(ferror(logger))
        return 1;
    return 0;
}

int close_logger()
{
    if(logger!=NULL)
    {
        int close_response = fclose(logger);
        logger=NULL;
        if(close_response==EOF)
        {
            return 1;
        }
    }
    return 0;
}

int clear_logger()
{
    if(logger==NULL) return 1;
    logger = freopen("logs/logger.log","w",logger);
    if(logger==NULL)
    {
        return 1;
    }
    logger = freopen("logs/logger.log","a+",logger);
    if(logger==NULL)
    {
        return init_logger();
    }
    return 0;
}
