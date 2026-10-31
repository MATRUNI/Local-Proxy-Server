#if defined(_WIN32)
#include "net_compat.h"
#include <windows.h>
#include <stdlib.h>
#include "poll_engine.h"

#define WSAPOLL_MAX_EVENTS 128

struct win_poll_engine {
    struct poll_engine base;
    WSAPOLLFD fds[ENGINE_MAX_EVENTS];
    void *user_data[ENGINE_MAX_EVENTS];
    int count;
};

static int wsapoll_engine_add(struct poll_engine *engine, int target, int flags, void *user_data)
{
    struct win_poll_engine *win = (struct win_poll_engine *)engine;
    if (win->count >= ENGINE_MAX_EVENTS) return -1;

    int idx = win->count++;
    win->fds[idx].fd = target;
    win->fds[idx].events = 0;
    if (flags & ENGINE_READ)  win->fds[idx].events |= POLLRDNORM;
    if (flags & ENGINE_WRITE) win->fds[idx].events |= POLLWRNORM;
    win->fds[idx].revents = 0;
    win->user_data[idx] = user_data;
    return 0;
}

static int wsapoll_engine_wait(struct poll_engine *engine, struct socket_info *events, int timeout)
{
    struct win_poll_engine *win = (struct win_poll_engine *)engine;
    if (win->count == 0) return 0;

    int res = WSAPoll(win->fds, win->count, timeout);
    if (res <= 0) return res;

    int fired = 0;
    for (int i = 0; i < win->count && fired < ENGINE_MAX_EVENTS; i++) {
        if (win->fds[i].revents != 0) {
            events[fired].user_data = win->user_data[i];
            events[fired].events = 0;
            if (win->fds[i].revents & (POLLRDNORM | POLLIN))
                events[fired].events |= ENGINE_READ;
            if (win->fds[i].revents & (POLLWRNORM | POLLOUT))
                events[fired].events |= ENGINE_WRITE;
            if (win->fds[i].revents & (POLLERR | POLLHUP))
                events[fired].events |= ENGINE_ERROR;
            fired++;
        }
    }
    return fired;
}

static int wsapoll_engine_mod(struct poll_engine *engine, int target, int flags, void *user_data)
{
    struct win_poll_engine *win = (struct win_poll_engine *)engine;

    for (int i = 0; i < win->count; i++) {
        if (win->fds[i].fd == (SOCKET)target) {
            short events = 0;
            if (flags & ENGINE_READ)  events |= POLLRDNORM;
            if (flags & ENGINE_WRITE) events |= POLLWRNORM;
            
            win->fds[i].events = events;
            win->fds[i].revents = 0;
            win->user_data[i] = user_data;
            return 0;
        }
    }
    return -1;
}

static int wsapoll_engine_del(struct poll_engine *engine, int target)
{
    struct win_poll_engine *win = (struct win_poll_engine *)engine;
    for (int i = 0; i < win->count; i++) {
        if (win->fds[i].fd == (SOCKET)target) {
            win->fds[i] = win->fds[win->count - 1];
            win->user_data[i] = win->user_data[win->count - 1];
            win->count--;
            return 0;
        }
    }
    return -1;
}

static int wsapoll_engine_destroy(struct poll_engine *engine)
{
    free(engine);
    return 0;
}

struct poll_engine *poll_engine_create(void)
{
    struct win_poll_engine *w = calloc(1, sizeof(struct win_poll_engine));
    if (w == NULL) return NULL;

    w->count = 0;
    
    w->base.add = wsapoll_engine_add;
    w->base.mod = wsapoll_engine_mod;
    w->base.del = wsapoll_engine_del;
    w->base.wait = wsapoll_engine_wait;
    w->base.destroy = wsapoll_engine_destroy;
    
    return &w->base;
}
#endif