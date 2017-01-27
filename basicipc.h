
#ifndef BASICIPC
#define BASICIPC

#include "shmipc.h"

#ifdef __cplusplus
extern "C" {
#endif

enum basicipc_request
{
  REQ_MALLOC = 0,
  REQ_FREE,
  REQ_MEMCPY,
  REQ_STRPUT,
  REQ_STRGET,
  REQ_MAX
};

shmipc_client_t basicipc_dial(char *filepath);
int basicipc_call(shmipc_client_t ipc, int reqid, ...);
int basicipc_send_async(shmipc_client_t ipc, int reqid, ...);

shmipc_service_t basicipc_init(char *filepath);

#ifdef __cplusplus
}
#endif

#endif  /* BASICIPC */
