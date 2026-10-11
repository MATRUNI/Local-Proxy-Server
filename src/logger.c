#include <stdio.h>
#include "logger.h"

int init_logger(FILE **logger)
{
    if(logger==NULL) return 1;
    if(logger!=NULL && *logger!=NULL)
    {
        return 0;
    }
    *logger = fopen("logs/logger.log","a+");
    if(*logger==NULL)
    {
        perror("fopen");
        printf("Failed to open file.\n");
        return 1;
    }
    return 0;
}

int write_log(FILE *logger,const char *line)
{
    if(logger==NULL || line==NULL) 
    {
        printf("Logger isn't availalbe.\n");
        return 1;
    }
    int res = fprintf(logger,"%s\n",line);
    if(res<0)
    {
        perror("fprintf");
        return 1;
    }
    int flush_response = fflush(logger);
    if(flush_response==0)
    return 0;
    return 1;
}

int get_logs(FILE *logger)
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

int close_logger(FILE **logger)
{
    if(logger!=NULL && *logger!=NULL)
    {
        int close_response = fclose(*logger);
        *logger=NULL;
        if(close_response==EOF)
        {
            return 1;
        }
    }
    return 0;
}

int clear_logger(FILE **logger)
{
    int res_close_logger = close_logger(logger);
    
    FILE *temp = fopen("logs/logger.log","w");
    if(temp==NULL)
    {
        perror("fopen clear");
        return 1;
    }
    if(res_close_logger==1)
    {
        if(write_log(temp,"Closing previous logger instance failed.")==1)
        {
            return 1;
        }
    }
    if(close_logger(&temp)==1)
    {
        return 1;
    }
    return init_logger(logger);
}
