#ifndef LOGGER_H
#define LOGGER_H
#include <stdio.h>

int init_logger();

int write_log(const char *line, ...);

int get_logs();

int clear_logger();

int close_logger();

#endif