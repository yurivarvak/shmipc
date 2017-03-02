

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
  if (!basicipc_call(ipc, REQ_STRGET, p, s, len))
    return strlen(s);
  // TODO: handle errors
  return 0;
}

int main()
{
  char *s1, *s2, buf[80];

  ipc = basicipc_dial("ipc.shm");

  assert(ipc && "can't init ipc");

  s1 = (char *)rem_malloc(200);
  rem_strput(s1, "some stuff");
  s2 = (char *)rem_malloc(80);
  rem_memcpy(s2, s1, strlen(s1)+1);
  rem_strget(s2, buf, sizeof(buf));
  assert(!strcmp(buf, s1));

  shmipc_close(ipc);

  return 0;
}
