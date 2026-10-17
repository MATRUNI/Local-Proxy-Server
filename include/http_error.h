#ifndef HTTP_ERROR_H
#define HTTP_ERROR_H

#include <stddef.h>

int format_error_response(char *buf, size_t max_len, int status_code, const char *message);

#endif
