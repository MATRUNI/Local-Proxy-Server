#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)|| defined(TEST_KQUEUE_ON_LINUX)

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>

#include "poll_engine.h"

static inline int translate_from_kqueue(int16_t filter, uint16_t flags)
{
    int res = 0;
    if (filter == EVFILT_READ)
        res |= ENGINE_READ;
    if (filter == EVFILT_WRITE)
        res |= ENGINE_WRITE;
    if (flags & EV_ERROR)
        res |= ENGINE_ERROR;
    return res;
}

static int kqueue_engine_add(struct poll_engine *engine, int target, int flags, void *user_data)
{
    struct kevent kev;
    int filter = (flags & ENGINE_READ) ? EVFILT_READ : EVFILT_WRITE;
    uint16_t kq_flags = EV_ADD | EV_ENABLE | EV_CLEAR;
    if (flags & ENGINE_ONESHOT) {
        kq_flags |= EV_ONESHOT;
    }
    EV_SET(&kev,target,filter, kq_flags, 0,0, user_data);
    return kevent(engine->fd,&kev,1,NULL, 0, NULL);
}

static int kqueue_engine_del(struct poll_engine *engine, int target)
{
    struct kevent kev;
    EV_SET(&kev, target, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(engine->fd, &kev, 1, NULL, 0, NULL);
    return 0;
}

static int kqueue_engine_wait(struct poll_engine *engine, struct socket_info *events, int timeout)
{
    struct timespec ts;
    struct timespec *pts = NULL;
    if (timeout >= 0) {
        ts.tv_sec = timeout / 1000;
        ts.tv_nsec = (timeout % 1000) * 1000000;
        pts = &ts;
    }
    struct kevent kq_events[ENGINE_MAX_EVENTS];
    int n = kevent(engine->fd, NULL, 0, kq_events, ENGINE_MAX_EVENTS, pts);
    for(int i=0;i<n;i++)
    {
        events[i].user_data = kq_events[i].udata;
        events[i].events = translate_from_kqueue(kq_events[i].filter, kq_events[i].flags);
    }
    return n;
}

static int kqueue_engine_destroy(struct poll_engine *engine) {
    close(engine->fd);
    free(engine);
    return 0;
}

struct poll_engine *poll_engine_create(void) {
    struct poll_engine *p = malloc(sizeof(struct poll_engine));
    if (p == NULL) return NULL;
    p->fd = kqueue();
    if (p->fd == -1) {
        free(p);
        return NULL;
    }
    p->add = kqueue_engine_add;
    p->mod = kqueue_engine_add;
    p->del = kqueue_engine_del;
    p->wait = kqueue_engine_wait;
    p->destroy = kqueue_engine_destroy;
    return p;
}

#endif
