#ifndef NET_COMPAT_H
#define NET_COMPAT_H
#include <stdint.h>
#include <time.h>
#if defined(_WIN32)
    #ifndef __USE_MINGW_ANSI_STDIO  
        #define __USE_MINGW_ANSI_STDIO 1
    #endif
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0600
    #endif
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <io.h>

    // 1. Socket handle type
    typedef SOCKET socket_t;

    // 2. Closing sockets
    #define close_socket(s) closesocket(s)

    // 3. Socket errors
    #define socket_errno WSAGetLastError()
    #define EWOULDBLOCK_COMPAT WSAEWOULDBLOCK
    #define EAGAIN_COMPAT      WSAEWOULDBLOCK
    #define EINPROGRESS_COMPAT WSAEWOULDBLOCK
    #define ECONNRESET_COMPAT WSAECONNRESET
    #define EPIPE_COMPAT      WSAECONNABORTED
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #include <netdb.h>

    // 1. In Unix, socket is just an int
    typedef int socket_t;

    // 2. Closing sockets
    #define close_socket(s) close(s)

    // 3. Socket errors
    #define socket_errno errno
    #define EWOULDBLOCK_COMPAT EWOULDBLOCK
    #define EAGAIN_COMPAT      EAGAIN
    #define EINPROGRESS_COMPAT EINPROGRESS
    #define ECONNRESET_COMPAT ECONNRESET
    #define EPIPE_COMPAT      EPIPE
#endif

static inline uint64_t get_time_ms(void) {
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
#endif
