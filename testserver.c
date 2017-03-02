
#include "basicipc.h"

#if defined(_WIN32)
#include <Windows.h>
unsigned sleep(unsigned seconds)
{
  Sleep(seconds*1000);
  return seconds;
}
#else
#include <unistd.h>
#endif

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
