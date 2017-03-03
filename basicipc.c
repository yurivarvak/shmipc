
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include "basicipc.h"

typedef union
{
  bool b;       /* 'B' */
  int32_t i32;  /* 'I' */
  int64_t i64;  /* 'L' */
  void *p;      /* 'P' */
  char *s;      /* 'S' */
  wchar_t *ws;  /* 'W' */
} basicipc_value;

#define MAX_IPC_ARGS 4
typedef struct
{
  int req_id;
  char args[MAX_IPC_ARGS+1];  /* arg types */
  char ret;                   /* return type */
} basicipc_reqdef;

static basicipc_reqdef basicipc_DD[] = {
  { REQ_MALLOC, "I", 'P'},
  { REQ_FREE, "P", 0},
  { REQ_MEMCPY, "PPI", 'P'},
  { REQ_STRPUT, "PS", 'B'},
  { REQ_STRGET, "PI", 'S'},
};

typedef struct {
  int reqid;
  basicipc_value args[MAX_IPC_ARGS];
  basicipc_value ret;
  int ret_size;
} client_request;

typedef struct {
  int reqid;
  client_request *clientdata;
  basicipc_value args[MAX_IPC_ARGS];
} wire_payload;

static int send_request(client_request *req, wire_payload *payload, int max_size)
{
  basicipc_reqdef *rdef = basicipc_DD + req->reqid;
  void *vardata = payload + 1;
  int vardatasize = max_size - sizeof(wire_payload);
  int vardataused = 0, next = 0;
  payload->reqid = req->reqid;
  payload->clientdata = req;
  for (int i = 0; i < strlen(rdef->args); i++)
  {
    switch (rdef->args[i]) {
    case 'I':
    case 'L':
    case 'P':
    case 'B':
      payload->args[i] = req->args[i];
      break;
    case 'S':
      next = vardataused + strlen(req->args[i].s);
      assert(next < vardatasize);           // TODO: need to handle insufficient buffer sizes
      payload->args[i].i32 = vardataused;   // store offset
      strcpy((char *)vardata + vardataused, req->args[i].s);
      vardataused = next;
      break;
    case 'W':
      next = vardataused + wcslen(req->args[i].ws) * sizeof(wchar_t);
      assert(next < vardatasize);           // TODO: need to handle insufficient buffer sizes
      payload->args[i].i32 = vardataused;   // store offset
      wcscpy((wchar_t *)((char *)vardata + vardataused), req->args[i].ws);
      vardataused = next;
      break;
    default: assert(0);
    }
  }
  return 0;  // all good...
}

static client_request *read_response(wire_payload *payload)
{
  basicipc_reqdef *rdef = basicipc_DD + payload->reqid;
  client_request *resp = payload->clientdata;
  void *vardata = payload + 1;
  switch (rdef->ret) {
    case 'I':
    case 'L':
    case 'P':
    case 'B':
      resp->ret = payload->args[0];
      break;
    case 'S':
      assert(payload->args[0].i32 >= 0);
      char *s = (char *)vardata + payload->args[0].i32;
      assert(strlen(s) <= resp->ret_size); // TODO: handle insufficient app buffer
      strcpy(resp->ret.s, s);
      break;
    case 'W':
      assert(payload->args[0].i32 >= 0);
      wchar_t *ws = (wchar_t *)((char *)vardata + payload->args[0].i32);
      assert(wcslen(ws) <= resp->ret_size); // TODO: handle insufficient app buffer
      wcscpy(resp->ret.ws, ws);
      break;
    case 0:  
      break;
    default: assert(0);
  }
  return resp;
}

shmipc_client_t basicipc_dial(char *filepath)
{
  return shmipc_dial(filepath, (encode_fn)send_request, (decode_fn)read_response);
}

int basicipc_call(shmipc_client_t ipc, int reqid, ...)
{
  assert(reqid >= 0 && reqid < REQ_MAX);
  basicipc_reqdef *rdef = basicipc_DD + reqid;
  client_request app;
  void *ret;
  va_list vl;

  va_start(vl,reqid);

  app.reqid = reqid;
  for (int i = 0; i < strlen(rdef->args); i++)
  {
    if (rdef->args[i] == 'I') app.args[i].i32 = va_arg(vl,int);
    else if (rdef->args[i] == 'P') app.args[i].p = va_arg(vl,void *);
    else if (rdef->args[i] == 'S') app.args[i].s = va_arg(vl,char *);
    else if (rdef->args[i] == 'W') app.args[i].ws = va_arg(vl,wchar_t *);
    else if (rdef->args[i] == 'B') app.args[i].b = va_arg(vl,bool);
    else if (rdef->args[i] == 'L') app.args[i].i64 = va_arg(vl,int64_t);
    else assert(0 && "unknown arg type");
  }

  if (rdef->ret == 'S')
  {
    app.ret.s = va_arg(vl,char *);
    app.ret_size = va_arg(vl,int);
  }
  else if (rdef->ret == 'W')
  {
    app.ret.ws = va_arg(vl,wchar_t *);
    app.ret_size = va_arg(vl,int);
  }
  else if (rdef->ret != 0) 
  {
    ret = va_arg(vl,void *);
    assert(ret);
  }

  va_end(vl);

  void *r = shmipc_call(ipc, &app);

  if (!r) // error
    return -1;  // maybe handle errors here...

  assert(r == &app);
  
  switch (rdef->ret) {
    case 'I': *(int*)ret = app.ret.i32; break;
    case 'L': *(int64_t*)ret = app.ret.i64; break;
    case 'P': *(void**)ret = app.ret.p; break;
    case 'B': *(bool*)ret = app.ret.b; break;
  }

  return 0;
}

int basicipc_send_async(shmipc_client_t ipc, int reqid, ...)
{
  assert(reqid >= 0 && reqid < REQ_MAX);
  basicipc_reqdef *rdef = basicipc_DD + reqid;
  client_request app;
  va_list vl;

  va_start(vl,reqid);

  app.reqid = reqid;
  for (int i = 0; i < strlen(rdef->args); i++)
  {
    if (rdef->args[i] == 'I') app.args[i].i32 = va_arg(vl,int);
    else if (rdef->args[i] == 'P') app.args[i].p = va_arg(vl,void *);
    else if (rdef->args[i] == 'S') app.args[i].s = va_arg(vl,char *);
    else if (rdef->args[i] == 'W') app.args[i].ws = va_arg(vl,wchar_t *);
    else if (rdef->args[i] == 'B') app.args[i].b = va_arg(vl,bool);
    else if (rdef->args[i] == 'L') app.args[i].i64 = va_arg(vl,int64_t);
    else assert(0 && "unknown arg type");
  }

  va_end(vl);

  return shmipc_send_async(ipc, &app);
}

// server stuff

#define ProcessedReq ((void *)-1)
#define ReqInWire(r) (((int64_t)r) < 0)

static void *read_request(wire_payload *payload)
{
  basicipc_value *args, *ret;
  assert(!ReqInWire(payload));
  ret = args = payload->args;
  switch (payload->reqid) {
  case REQ_MALLOC: ret->p = malloc(args[0].i32); break;
  case REQ_FREE: free(args[0].p); break;
  case REQ_MEMCPY: ret->p = memcpy(args[0].p, args[1].p, args[2].i32); break;
  case REQ_STRPUT: {
    char *trg = (char *)args[0].p;
    char *src = (char *)(payload + 1) + args[1].i32;
    ret->b = trg ? strcpy(trg, src) != 0 : false;
    break; }
  case REQ_STRGET: {
    char *trg = (char *)(payload + 1);
    char *src = (char *)args[0].p;
    strncpy(trg, src, args[1].i32);
    ret->i32 = 0;
    break; }
  }
  return ProcessedReq;
}

static void process_async_request (void *req)
{
  assert(req == ProcessedReq);
}

static void *process_request(void *req)
{
  assert(req == ProcessedReq);
  return req;
}

static int send_response(void *resp, void *payload, int max_len)
{
  assert(resp == ProcessedReq);
  return 0;
}

shmipc_service_t basicipc_init(char *filepath)
{
  return shmipc_init(filepath, send_response, (decode_fn)read_request, process_request, process_async_request);
}

