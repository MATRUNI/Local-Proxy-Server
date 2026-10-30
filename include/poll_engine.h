#ifndef POLL_ENGINE_H
#define POLL_ENGINE_H
#include "logger.h"

enum POLL_EVENTS {
    ENGINE_READ = 1<<0,
    ENGINE_WRITE = 1<<1,
    ENGINE_ERROR = 1<<2,
    ENGINE_ONESHOT = 1<<3
};

struct socket_info {
    void *user_data;
    int events;
};

struct poll_engine{
    int fd;
    int (*add)(struct poll_engine *engine, int target, int flags, void *user_data);
    int (*mod)(struct poll_engine *engine, int target, int flags, void *user_data);
    int (*del)(struct poll_engine *engine, int target);
    int (*wait)(struct poll_engine *engine, struct socket_info *events, int timeout);
    int (*destroy)(struct poll_engine *engine);
};

struct poll_engine *poll_engine_create(void);

#endif