#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#if defined(__linux__)
    #include <unistd.h>
    #include <sys/syscall.h>
#elif defined(__APPLE__)
    #include <pthread.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

#define COLOR_RESET   "\033[0m"
#define COLOR_CYAN    "\033[1;36m"   // Bright Cyan for DEBUG
#define COLOR_GREEN   "\033[1;32m"   // Bright Green for INFO
#define COLOR_YELLOW  "\033[1;33m"   // Bright Yellow for WARN
#define COLOR_RED     "\033[1;31m"   // Bold Red for ERROR
#define COLOR_BLUE    "\033[1;34m"   // Blue for Thread ID

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
#include "config.h"

static FILE * logger = NULL;

static pthread_mutex_t logger_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t logger_thread_cond = PTHREAD_COND_INITIALIZER;
#define BUCKET_BUFFER_SIZE (64 * 1024)
static char BUCKET_A[BUCKET_BUFFER_SIZE];
static char BUCKET_B[BUCKET_BUFFER_SIZE];
static char *active_buffer = BUCKET_A;
static char *waiting_buffer = BUCKET_B;
static size_t active_buf_size = 0;
static size_t waiting_buf_size = 0;
static volatile int logger_running = 0;
static pthread_t tid;

static log_level_t current_log_level = LOG_INFO;

static const char *log_value[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

void change_log_level(int new_level)
{
    if(new_level<0 || new_level>3) return;
    current_log_level = new_level;
}

void *logger_thread(void *arg)
{
    (void)arg;
    while (logger_running)
    {
        if(pthread_mutex_lock(&logger_mutex)!=0)
        {
            continue;
        }

        while(active_buf_size==0 && logger_running)
        {
            pthread_cond_wait(&logger_thread_cond,&logger_mutex);
        }
        char *temp = active_buffer;
        active_buffer = waiting_buffer;
        waiting_buffer = temp;
        waiting_buf_size = active_buf_size;
        active_buf_size = 0;
        pthread_mutex_unlock(&logger_mutex);
        if(waiting_buf_size>0 && logger!=NULL)
        {
            fwrite(waiting_buffer,1,waiting_buf_size,logger);
            fflush(logger);
        }
    }
    return NULL;
}

int init_logger()
{
    if(logger!=NULL)
    {
        return 0;
    }
    logger = fopen("logs/logger.log","a+");
    logger_running = 1;
    if(logger==NULL)
    {
        perror("fopen");
        printf("Failed to open file.\n");
        return 1;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr,LOGGER_THREAD_STACK_SIZE);
    if(pthread_create(&tid,&attr,logger_thread,NULL)!=0)
    {
        fclose(logger);
        printf("Out of theads.\n");
        return 1;
    }
    pthread_attr_destroy(&attr);
    return 0;
}

int write_log(log_level_t level,const char *line,...)
{
    if(logger==NULL || line==NULL) 
    {
        printf("Logger isn't availalbe.\n");
        return 1;
    }
    if(level<current_log_level) return 0;
    if(level<0 || level>3)
        level = 1;
    int return_value = 1;
    struct timespec ts;
    struct tm tm;
    char timestamp[32];
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
        return return_value;

    if (localtime_r(&ts.tv_sec, &tm) == NULL)
        return return_value;

    if (strftime(timestamp, sizeof(timestamp),"%Y-%m-%d %H:%M:%S", &tm) == 0)
        return return_value;

    va_list args;
    uint64_t tid = get_thread_id();
    
    char msg[1024];
    int len = 0;
    
    len = snprintf(msg,sizeof(msg),"[%s.%03ld] [TID:%lu] [%s] ",timestamp,ts.tv_nsec/1000000,tid,log_value[level]);
    
    if(len<0)
    {
        perror("snprintf");
        return return_value;
    }
    va_start(args,line);

    int res = vsnprintf(msg+len,sizeof(msg)-len,line,args);

    va_end(args);
    if(res<0)
    {
        perror("vsnprintf");
        return return_value;
    }
    len+=res;
    if ((size_t)len < sizeof(msg) - 1) {
        msg[len++] = '\n';
    }
    int lock_res = pthread_mutex_lock(&logger_mutex);
    if (lock_res != 0)
        return 1;
    if(active_buf_size+len<BUCKET_BUFFER_SIZE)
    {
        memcpy(active_buffer + active_buf_size, msg, len);
        active_buf_size += len; 

        pthread_cond_signal(&logger_thread_cond);
        return_value = 0;
    }

    pthread_mutex_unlock(&logger_mutex);
    return return_value;
}

int get_logs()
{
    if(logger==NULL) return 1;
    rewind(logger);
    char line[1024];

    while (fgets(line, sizeof(line), logger) != NULL)
    {
        char *tid_start = strstr(line, "[TID:");
        char *tid_end = (tid_start != NULL) ? strchr(tid_start, ']') : NULL;

        char *tag = NULL;
        const char *tag_color = COLOR_RESET;
        int tag_len = 0;

        if ((tag = strstr(line, "[ERROR]")) != NULL) {
            tag_color = COLOR_RED;
            tag_len = 7;
        } else if ((tag = strstr(line, "[WARN]")) != NULL) {
            tag_color = COLOR_YELLOW;
            tag_len = 6;
        } else if ((tag = strstr(line, "[INFO]")) != NULL) {
            tag_color = COLOR_GREEN;
            tag_len = 6;
        } else if ((tag = strstr(line, "[DEBUG]")) != NULL) {
            tag_color = COLOR_CYAN;
            tag_len = 7;
        }

        if (tid_start != NULL && tid_end != NULL && tag != NULL) {

            printf(COLOR_RESET "%.*s" COLOR_RESET, (int)(tid_start - line), line);

            printf(COLOR_BLUE "%.*s" COLOR_RESET " ", (int)(tid_end - tid_start + 1), tid_start);
            printf("%s%.*s" COLOR_RESET, tag_color, tag_len, tag);

            printf("%s", tag + tag_len);
        } else {
            printf("%s", line);
        }
    }

    if(ferror(logger))
        return 1;
    return 0;
}


int close_logger()
{
    if(logger!=NULL)
    {
        logger_running = 0;
        pthread_cond_signal(&logger_thread_cond);
        pthread_join(tid,NULL);
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
