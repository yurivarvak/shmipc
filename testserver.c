
#include <unistd.h>
#include "basicipc.h"

int main()
{
  shmipc_service_t ipc;

  while (!(ipc = basicipc_init("ipc.shm")))
    sleep(1);

  while (!shmipc_run(ipc))
    sleep(1);

  shmipc_shutdown(ipc);

  return 0;
}
