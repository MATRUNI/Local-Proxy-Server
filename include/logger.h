#ifndef LOGGER_H
#define LOGGER_H
#include <stdio.h>

int init_logger(FILE **logger);

int write_log(FILE *logger,const char *line);

int get_logs(FILE *logger);

int clear_logger(FILE **logger);

int close_logger(FILE **logger);

#endif