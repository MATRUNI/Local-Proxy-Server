#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

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

static uint64_t get_thread_id(void)
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

#define FILE_COUNT (5)
#define MAX_FILE_SIZE (2*1024*1024)
#define EMERGENCY_MAX_SIZE    (10 * 1024 * 1024)
static int current_slot = 0;
static size_t current_file_size = 0;
static bool overflow_mode = false;

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

static void get_log_filepath(char *buf, size_t size, int slot, bool is_active, bool is_stress)
{
    const char *active_prefix = is_active ? "@" : "";
    const char *stress_prefix = is_stress ? "~" : "";
    snprintf(buf, size, "logs/%s%slogger_%d.log", active_prefix, stress_prefix, slot);
}

static int spawn_logger_x(char *buf)
{
    FILE *lg = fopen(buf,"w");
    if(lg==NULL) return 1;
    fclose(lg);
    return 0;
}

// first @, 2nd ~ stress log

static void last_active_file(DIR *dir,char *file_name, size_t *file_size)
{
    struct stat st;
    struct dirent *entry;
    char at_files[FILE_COUNT][300];
    int slot[FILE_COUNT];
    size_t at_sizes[FILE_COUNT];
    int count = 0;
    int max_index=-1;

    while((entry = readdir(dir))!=NULL)
    {
        if(entry->d_name[0]=='@'    && count < FILE_COUNT)
        {
            sscanf(entry->d_name,"@logger_%d.log",&slot[count]);
            if(max_index<0)
            {
                max_index=count;
            }
            else
            {
                if(slot[max_index]==FILE_COUNT-1 && slot[count]==0)
                {
                    max_index = count;
                }
                else if(slot[max_index] == 0 && slot[count] == FILE_COUNT-1){}
                else
                {
                    max_index = (slot[count]>slot[max_index]) ? count : max_index;
                }
            }
            
            snprintf(at_files[count],300,"logs/%s",entry->d_name);
            if(stat(at_files[count],&st)==0)
            {
                at_sizes[count] = (size_t) st.st_size;
            }
            else
                perror("stat");
            count++;
        }
    }
    if(count==0)
        return;
    else if(count==1)
    {
        strcpy(file_name,at_files[0]);
        *file_size = at_sizes[0];
        return;
    }
    for(int i=0;i<count;i++)
    {
        if(i==max_index) continue;
        char clean[300];
        get_log_filepath(clean, sizeof(clean), slot[i], false, false);
        rename(at_files[i], clean);
    }
    strcpy(file_name,at_files[max_index]);
    *file_size = at_sizes[max_index];
}

static int clear_logger(char *name)
{
    FILE *temp = fopen(name,"w");
    if(temp==NULL)
    {
        return 1;
    }
    fclose(temp);
    return 0;
}

static bool file_X_exist(char *name)
{
    if (access(name,F_OK) == 0) {
        return true;
    }
    return false;
}

static int open_file(char*file_name)
{
    if(file_name==NULL) return 1;
    logger = fopen(file_name,"a+");
    logger_running = 1;
    if(logger==NULL)
    {
        perror("fopen");
        printf("Failed to open file.\n");
        return 1;
    }
    return 0;
}

static int get_next_logger_file()
{
    int next_slot = (current_slot + 1 >= FILE_COUNT) ? 0 : current_slot + 1;

    char next_active[300], next_archived[300];
    get_log_filepath(next_active, sizeof(next_active), next_slot, true, false);
    get_log_filepath(next_archived, sizeof(next_archived), next_slot, false, false);

    char curr_active[300], curr_archived[300];
    get_log_filepath(curr_active, sizeof(curr_active), current_slot, true, false);
    get_log_filepath(curr_archived, sizeof(curr_archived), current_slot, false, false);

    if (file_X_exist(next_archived))
    {
        if (rename(next_archived, next_active) != 0 || clear_logger(next_active) != 0)
        {
            fprintf(stderr, "[LOGGER ALERT] Failed to rotate log file! Operating in overflow mode.\n");
            overflow_mode = true;
            return 1;
        }
    }
    else
    {
        if (spawn_logger_x(next_active) != 0)
        {
            fprintf(stderr, "[LOGGER ALERT] Failed to spawn next log file! Operating in overflow mode.\n");
            overflow_mode = true;
            return 1;
        }
    }
    if (logger != NULL)
    {
        fclose(logger);
        logger = NULL;
    }
    if (open_file(next_active) != 0)
    {
        open_file(curr_active);
        overflow_mode = true;
        return 1;
    }
    rename(curr_active, curr_archived);
    current_slot = next_slot;
    current_file_size = 0;
    return 0;
}

static void *logger_thread(void *arg)
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
            current_file_size+=waiting_buf_size;
            fflush(logger);
            if(current_file_size>=MAX_FILE_SIZE)
            {
                if(!overflow_mode)
                {
                    if (get_next_logger_file() != 0)
                    {
                        overflow_mode = true;
                    }
                }
                else if (current_file_size >= EMERGENCY_MAX_SIZE)
                {
                    fprintf(stderr, "[LOGGER CRITICAL] 10 MB emergency limit hit! Truncating to protect disk.\n");
                    char active_name[300];
                    get_log_filepath(active_name, sizeof(active_name), current_slot, true, false);
                    clear_logger(active_name);
                    current_file_size = 0;
                    overflow_mode = false;
                }
            }
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
    DIR *dir;
    char file_name[300]="";
    dir = opendir("logs");
    if(dir==NULL)
    {
        printf("Failed to open directory.\n");
        return 1;
    }
    last_active_file(dir,file_name,&current_file_size);
    if(file_name[0]=='\0')
    {
        get_log_filepath(file_name, sizeof(file_name), current_slot, true, false);
        if(spawn_logger_x(file_name)==1)
        {
            printf("Failed to create logger file.\n");
            return 1;
        }
        if(open_file(file_name)==1)
        {
            return 1;
        }
    }
    else
    {
        sscanf(file_name,"logs/@logger_%d.log",&current_slot);
        if(current_file_size >= MAX_FILE_SIZE)
        {
            get_next_logger_file();
            get_log_filepath(file_name, sizeof(file_name), current_slot, true, false);
        }
        if(open_file(file_name)==1)
        {
            return 1;
        }
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
    closedir(dir);
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
