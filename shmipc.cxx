

// semaphores
#include <Windows.h>

typedef HANDLE IPCSem;
typedef HANDLE TProc;
typedef DWORD TProcID;

IPCSem Sem_create()
{
  return CreateSemaphore(NULL, 0, MAXLONG, NULL);
}
void Sem_destroy(IPCSem sem)
{
  CloseHandle(sem);
}
void Sem_wait(IPCSem sem)
{
  WaitForSingleObject(sem, INFINITE);
}
bool Sem_trywait(IPCSem sem)
{
  return WaitForSingleObject(sem, 0) == WAIT_OBJECT_0;
}
bool Sem_timedwait(IPCSem sem, int wait_msec)
{
  return WaitForSingleObject(sem, wait_msec) == WAIT_OBJECT_0;
}
void Sem_post(IPCSem sem)
{
  ReleaseSemaphore(sem, 1, NULL);
}

#include <atomic>

typedef char aling64bytes[64];

struct IPCControl
{
  enum State { 
    NotUsed,	
    ClientSend, ClientWait, ServerRecv, ServerDone,   // sync request
    ClientAsync, ServerAsync,                         // async request
    CommErr };
  union { struct {
  std::atomic<State> state;
  IPCSem ready; 
  }; aling64bytes align; };
};

#define IPC_SYNC_PAYLOAD_SIZE (1024*64)
struct IPCSyncMessage
{
  IPCControl control;
  char payload[IPC_MESSAGE_PAYLOAD_SIZE];
};

#define IPC_ASYNC_PAYLOAD_SIZE (1024)
struct IPCAsyncMessage
{
  IPCControl control;
  char payload[IPC_ASYNC_PAYLOAD_SIZE];
};

#define IPC_CHANNEL_SYNC_MESSAGES 4
#define IPC_CHANNEL_ASYNC_MESSAGES 64
struct IPCChannel
{
union { struct {
  TProcID owner_proc;
  IPCSem stuff_to_do;
}; aling64bytes align; };
  IPCSyncMessage s_msgs[IPC_CHANNEL_SYNC_MESSAGES];
  IPCAsyncMessage a_msgs[IPC_CHANNEL_ASYNC_MESSAGES];
};

bool time_to_wait(int64_t count)
{
  return count > 1;
}


