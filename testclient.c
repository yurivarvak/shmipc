

#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include "basicipc.h"

static shmipc_client_t ipc;

void *rem_malloc(int size)
{
  void *ret;
  if (!basicipc_call(ipc, REQ_MALLOC, size, &ret))
    return ret;
  // TODO: handle errors
  return 0;
}

void rem_free(void *p)
{
  if (!basicipc_send_async(ipc, REQ_FREE, p))
    return;
  // TODO: handle errors
}

void *rem_memcpy(void *target, void *source, int size)
{
  void *ret;
  if (!basicipc_call(ipc, REQ_MEMCPY, target, source, size, &ret))
    return ret;
  // TODO: handle errors
  return 0;
}

bool rem_strput(void *p, char *s)
{
  bool ret;
  if (!basicipc_call(ipc, REQ_STRPUT, p, s, &ret))
    return ret;
  // TODO: handle errors
  return 0;
}

int rem_strget(void *p, char *s, int len)
{
  if (!basicipc_call(ipc, REQ_STRGET, p, len, s, len+1))
    return strlen(s);
  // TODO: handle errors
  return 0;
}

#if defined(_WIN32)
#include <Windows.h>
#define CLOCK_REALTIME 0
struct timespec { long tv_sec; long tv_nsec; };    
static int clock_gettime(int, struct timespec *spec)
{
  __int64 wintime; GetSystemTimeAsFileTime((FILETIME*)&wintime);
  wintime -= 116444736000000000i64; 
  spec->tv_sec = wintime / 10000000i64; 
  spec->tv_nsec = wintime % 10000000i64 * 100;  
  return 0;
}
#else
#include <time.h>
#endif

#include <stdint.h>
#include <stdio.h>

int64_t diff_ts(struct timespec *ts, struct timespec *te) 
{
  int64_t res = te->tv_sec - ts->tv_sec;
  res *= 1000000000; /* to ns */
  return res + te->tv_nsec - ts->tv_nsec;
}

int main()
{
  char *str = "some stuff";
  char *s1, *s2, buf[80];
  struct timespec t1, t2;

  ipc = basicipc_dial("ipc.shm");

  assert(ipc && "can't init ipc");

  clock_gettime(CLOCK_REALTIME, &t1);

  s1 = (char *)rem_malloc(200);
  rem_strput(s1, str);
  s2 = (char *)rem_malloc(80);
  rem_memcpy(s2, s1, strlen(str)+1);
  rem_strget(s2, buf, sizeof(buf));
  assert(!strcmp(buf, str));
  rem_free(s2);
  rem_free(s1);

  clock_gettime(CLOCK_REALTIME, &t2);

  printf("diff time (ns): %lld\n", diff_ts(&t1, &t2));

  shmipc_close(ipc);

  return 0;
}
