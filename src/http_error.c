#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "http_error.h"

static const char *get_status_text(int code)
{
    switch (code)
    {
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Error";
    }
}

int format_error_response(char *buf, size_t max_len, int status_code, const char *message)
{
    if (buf == NULL) return -1;

    const char *status_text = get_status_text(status_code);
    const char *body = (message != NULL) ? message : status_text;
    size_t body_len = strlen(body);

    int written = snprintf(buf, max_len,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: *\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status_code, status_text, ( unsigned long)body_len, body
    );

    if (written < 0 || (size_t)written >= max_len)
        return -1;

    return written;
}
