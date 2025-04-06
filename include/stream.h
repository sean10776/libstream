#ifndef STREAM_H
#define STREAM_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>

#ifdef WIN32
#include <winsock2.h>
#include <windows.h>
#include <WS2tcpip.h>
#else
#include <pthread.h>
#include <sys/select.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#ifdef WIN32
typedef HANDLE str_thread_t;
typedef CRITICAL_SECTION str_lock_t;
#else
typedef pthread_t str_thread_t;
typedef pthread_mutex_t str_lock_t;
#endif

typedef enum {
    STR_NONE,
    STR_FILE,
    STR_SERIAL,
    STR_TCPSVR,
    STR_TCPCLI,
    STR_TCPCLI_SSL,
    STR_UDPSVR,
    STR_UDPCLI,
    STR_NTRIPCLI,
    STR_NTRIPSVR,
} stream_type;

typedef enum {
    STR_MODE_NONE = 0,
    STR_MODE_R    = 1,
    STR_MODE_W    = 2,
    STR_MODE_RW   = 3,
    STR_MODE_A    = 4, // only for STR_FILE
} stream_mode;

#define MAXSTRPATH  1024        /* max length of stream path */
#define MAXSTRMSG   1024        /* max length of stream message */
#define NTRIP_AGENT "libstream/0.1"

typedef struct {            /* stream type */
    stream_type type;       /* type (STR_???) */
    stream_mode mode;       /* mode (STR_MODE_?) */
    int state;              /* state (-1:error,0:close,1:open) */
    uint32_t inb,inr;       /* input bytes/rate */
    uint32_t outb,outr;     /* output bytes/rate */
    uint32_t tick_i;        /* input tick tick */
    uint32_t tick_o;        /* output tick */
    uint32_t tact;          /* active tick */
    uint32_t inbt,outbt;    /* input/output bytes at tick */
    str_lock_t lock;            /* lock flag */
    void *port;             /* type dependent port control struct */
    char path[MAXSTRPATH];  /* stream path */
    char msg [MAXSTRMSG];   /* stream message */
} stream_t;

EXPORT void strinitcom(void);
EXPORT void strinit(stream_t *stream);
EXPORT int  stropen(stream_t *stream, stream_type type, stream_mode mode,
                    const char *path);
EXPORT void strclose(stream_t *stream);
EXPORT int  strread(stream_t *stream, unsigned char *buff, int n);
EXPORT int  strwrite(stream_t *stream, unsigned char *buff, int n);
EXPORT int  strstat(stream_t *stream, char *msg);
EXPORT int  strstatx(stream_t *stream, char *msg);
EXPORT void strlock  (stream_t *stream);
EXPORT void strunlock(stream_t *stream);

#ifdef __cplusplus
}
#endif

#endif // STREAM_H