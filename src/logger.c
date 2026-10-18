#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>

#if defined(__linux__)
    #include <unistd.h>
    #include <sys/syscall.h>
#elif defined(__APPLE__)
    #include <pthread.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

uint64_t get_thread_id(void)
{
    #if defined(__linux__)
        return (uint64_t)syscall(SYS_gettid);
    #elif defined(__APPLE__)
        uint64_t tid;
        pthread_threadid_np(NULL, &tid);
        return tid;
    #elif defined(_WIN32)
        return (uint64_t)GetCurrentThreadId();
    #else
        return (uint64_t)(uintptr_t)pthread_self();
    #endif
}

#include "logger.h"

static FILE * logger = NULL;
static pthread_mutex_t logger_mutex = PTHREAD_MUTEX_INITIALIZER;

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

    struct timespec ts;
    struct tm tm;
    char timestamp[32];
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
        return 1;

    if (localtime_r(&ts.tv_sec, &tm) == NULL)
        return 1;

    if (strftime(timestamp, sizeof(timestamp),"%Y-%m-%d %H:%M:%S", &tm) == 0)
        return 1;

    int lock_res = pthread_mutex_lock(&logger_mutex);
    if (lock_res != 0)
        return 1;

    va_list args;
    va_start(args,line);
    uint64_t tid = get_thread_id();
    if(fprintf(logger,"[%s.%03ld] [TID:%lu] ",timestamp,ts.tv_nsec/1000000,tid)<0)
    {
        perror("fprintf");
        goto failed;
    }
    int res = vfprintf(logger,line,args);

    va_end(args);
    if(res<0)
    {
        perror("vfprintf");
        goto failed;
    }
    res = fputc('\n',logger);
    if(res==EOF)
    {
        perror("fputc");
        goto failed;
    }

    int flush_response = fflush(logger);
    if(flush_response==0)
    {
        pthread_mutex_unlock(&logger_mutex);
        return 0;
    }
    failed:
    pthread_mutex_unlock(&logger_mutex);
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
