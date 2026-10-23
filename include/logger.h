#ifndef LOGGER_H
#define LOGGER_H
#include <stdio.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
} log_level_t;

void change_log_level(int new_level);

int init_logger();

int write_log(log_level_t level,const char *line, ...);

int get_logs();

int close_logger();

#endif