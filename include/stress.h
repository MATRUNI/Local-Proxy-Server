#ifndef STRESS_H
#define STRESS_H

#include <stddef.h>

struct stress_config {
    char target_url[1024];
    char method[16];          // "GET", "POST", etc.
    char body[8192];          // Custom user payload
    size_t body_len;
    int concurrency;          // Parallel connections (e.g. 100)
    long total_requests;      // Total requests to send (e.g. 20000)
    int threads;              // Worker threads (e.g. 4)
};
struct proxy_config;
int run_stress_engine(struct proxy_config *config);

#endif
