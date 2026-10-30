#if defined(__linux__)
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "poll_engine.h"

static inline uint32_t translate_to_epoll(int flags)
{
    uint32_t ep_events = EPOLLET;

    if (flags & ENGINE_READ)
        ep_events |= EPOLLIN;

    if (flags & ENGINE_WRITE)
        ep_events |= EPOLLOUT;

    if (flags & ENGINE_ERROR)
        ep_events |= (EPOLLERR | EPOLLHUP);

    if (flags & ENGINE_ONESHOT)
        ep_events |= EPOLLONESHOT;

    return ep_events;
}

static inline int translate_from_epoll(uint32_t ep_events)
{
    int flags = 0;

    if (ep_events & EPOLLIN)
        flags |= ENGINE_READ;

    if (ep_events & EPOLLOUT)
        flags |= ENGINE_WRITE;

    if (ep_events & (EPOLLERR | EPOLLHUP))
        flags |= ENGINE_ERROR;

    return flags;
}

static int epoll_engine_add(struct poll_engine *engine, int target, int flags, void *user_data)
{
    struct epoll_event ev = {
        .data.ptr = user_data,
        .events = translate_to_epoll(flags)
    };
    return epoll_ctl(engine->fd,EPOLL_CTL_ADD,target,&ev);
}

static int epoll_engine_mod(struct poll_engine *engine, int target, int flags, void *user_data)
{
    struct epoll_event ev = {
        .data.ptr = user_data,
        .events = translate_to_epoll(flags)
    };
    return epoll_ctl(engine->fd,EPOLL_CTL_MOD,target,&ev);
}

static int epoll_engine_del(struct poll_engine *engine, int target)
{
    return epoll_ctl(engine->fd,EPOLL_CTL_DEL,target,NULL);
}

static int epoll_engine_wait(struct poll_engine *engine, struct socket_info *events, int timeout)
{
    struct epoll_event ep_events[ENGINE_MAX_EVENTS];
    int n = epoll_wait(engine->fd,ep_events,ENGINE_MAX_EVENTS,timeout);
    for(int i=0;i<n;i++)
    {
        events[i].user_data = ep_events[i].data.ptr;
        events[i].events = translate_from_epoll(ep_events[i].events);
    }
    return n;
}

static int epoll_engine_destroy(struct poll_engine *engine)
{
    close(engine->fd);
    free(engine);
    return 0;
}

struct poll_engine *poll_engine_create(void)
{
    struct poll_engine *p = malloc(sizeof(struct poll_engine));
    if(p==NULL) return NULL;
    p->fd = epoll_create1(0);
    if(p->fd==-1)
    {
        free(p);
        return NULL;
    }
    p->add = epoll_engine_add;
    p->del = epoll_engine_del;
    p->mod = epoll_engine_mod;
    p->wait = epoll_engine_wait;
    p->destroy = epoll_engine_destroy;
    return p;
}
#endif