
#include <atomic>
#include <cassert>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <time.h>
#include <errno.h>

#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include "shmipc.h"

#if defined(_WIN32)
#define OSAPI WINDOWS
#elif defined(__MACH__)
#define OSAPI MACOS
#elif defined(__unix__)
#define OSAPI POSIX
#else
#error "unknown OS"
#endif

// semaphores

#if OSAPI == WINDOWS

#include <Windows.h>

struct IPCSem
{
  HANDLE sem;
  DWORD  owner_proc;
  bool   init;
  IPCSem() : sem(0), owner_proc(0), init(false) {}
  IPCSem(const IPCSem &other)
  {
    assert(other.init);
    owner_proc = GetCurrentProcessId();
    if (owner_proc == other.owner_proc)  // same process
    {
      sem = other.sem;
    }
    else
    {
      HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, 0, other.owner_proc);
      BOOL ok = DuplicateHandle(other.owner_proc, other.sem, owner_proc, &sem, 0, 0, DUPLICATE_SAME_ACCESS);
      assert(ok);
    }
    init = true;
  }
  ~IPCSem() { if (init) Destroy(); }
  void Init()
  {
    assert(!init);
    owner_proc = GetCurrentProcessId();
    sem = CreateSemaphore(NULL, 0, MAXLONG, NULL);
    init = true;
  }
  void Destroy()
  {
    assert(init);
    owner_proc = 0;
    CloseHandle(sem);
    init = false;
  }
  bool Wait(int msec = -1)
  {
    assert(init);
    DWORD res = WaitForSingleObject(sem, msec < 0 ? INFINITE : msec);
    assert(res != WAIT_FAILED);
    return res == WAIT_OBJECT_0;
  }
  void Post()
  {
    assert(init);
    ReleaseSemaphore(sem, 1, NULL);
  }
};

#elif OSAPI == MACOS
#error "Not implemeted for Mac yet"
#else   // POSIX

#include <semaphore.h>

struct IPCSem
{
  sem_t  holder;
  sem_t *sem;
  bool   init;
  bool   own;
  IPCSem() : init(false) {}
  IPCSem(const IPCSem &other)
  {
    assert(other.init);
    sem = other.sem;
    own = false;
    init = true;
  }
  ~IPCSem() { if (init) Destroy(); }
  void Init()
  {
    assert(!init);
    sem = &holder;
    if(sem_init(sem, 1, 0) == -1)
      assert(0);
    init = own = true;
  }
  void Destroy()
  {
    assert(init);
    if (own && sem_destroy(sem) == -1)
      assert(0);  // need to handle EBUSY
    init = false;
  }
  bool Wait(int msec = -1)
  {
    assert(init);
    int res;
    struct timespec ts;
    if (msec > 0)
    {
      res = clock_gettime(CLOCK_REALTIME, &ts);
      assert(res != -1);
      int64_t inc = (int64_t)msec * 1000000 + ts.tv_nsec;  // nanosecs
      ts.tv_sec += (time_t)(inc / 1000000);
      ts.tv_nsec = (long)(inc % 1000000);
    }
    do {
      if (msec < 0)
        res = sem_wait(sem);
      else if (msec == 0)
        res = sem_trywait(sem);
      else
        res = sem_timedwait(sem, &ts);
    } while (res == -1 && errno == EINTR);
    assert(!res || errno == ETIMEDOUT || errno == EAGAIN);
    return !res;
  } 
  void Post()
  {
    assert(init);
    if (sem_post(sem) == -1)
      assert(0);
  }
};

#endif


// Common IPC structures

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

struct IPCMessage
{
  IPCControl control;
};

#define IPC_SYNC_PAYLOAD_SIZE (1024*64)
struct IPCSyncMessage : public IPCMessage
{
  char payload[IPC_MESSAGE_PAYLOAD_SIZE];
};

#define IPC_ASYNC_PAYLOAD_SIZE (1024)
struct IPCAsyncMessage : public IPCMessage
{
  char payload[IPC_ASYNC_PAYLOAD_SIZE];
};

#define IPC_CHANNEL_SYNC_MESSAGES 4
#define IPC_CHANNEL_ASYNC_MESSAGES 64
struct IPCChannel
{
union {
  IPCSem stuff_to_do;
  aling64bytes align; 
};
  IPCSyncMessage s_msgs[IPC_CHANNEL_SYNC_MESSAGES];
  IPCAsyncMessage a_msgs[IPC_CHANNEL_ASYNC_MESSAGES];
};

bool time_to_wait(int64_t count)
{
  return count > 1;
}

// service
struct Service
{
  struct Thread
  {
    std::atomic<bool> terminate;  // service is asking thread to exit
	std::atomic<bool> terminated; // service has terminated a thread
	std::atomic<bool> completed;  // thread has finished running
	std::atomic<int>  idle_runs;
    std::thread *thr;
	Service *service;
	IPCMessage *msg;
	bool primary;
    Thread(Service *s, bool p = false) : 
	  service(s), terminate(false), terminated(false), completed(false), idle_runs(0), msg(0), primary(p)
	{ thr = new std::thread(RunThread, this); thr->detach(); }
	~Thread() { delete thr; }
	bool IsIdle() { return idle_runs > 2; }  // doing nothing for a while
  private:
    static void RunThread(Thread *th) { th->Run(); }
    void Run()
	{
	  int64_t local_runs = 0;

	  while (!terminate)
	  {
        bool async = false;
        bool got_something = false;
		local_runs++;
        
		if (time_to_wait(local_runs)) // nothing to do
		{
		  got_something = service->channel->stuff_to_do.Wait(1000);
		  if (!got_something)
		  {
		    idle_runs++;
		    if (!primary && IsIdle())
		      terminate = true;
		  }
		}
		else
		  got_something = service->channel->stuff_to_do.Wait(0);
		
		if (!got_something)  // no pending request
		{
		  std::this_thread::yield();
		  continue;
		}
		
		// acquire the request
		for (int i = 0; i < IPC_CHANNEL_SYNC_MESSAGES && !msg; i++)
		{
		  msg = service->channel->s_msgs + i;
		  IPCControl::State s = IPCControl::ClientWait;
		  if (!msg->control.state.compare_exchange_strong(s, IPCControl::ServerRecv))
		    msg = 0;
		}
		
        for (int i = 0; i < IPC_CHANNEL_ASYNC_MESSAGES && !msg; i++)
        {
          msg = service->channel->a_msgs + i;
          IPCControl::State s = IPCControl::ClientAsync;
          if (!msg->control.state.compare_exchange_strong(s, IPCControl::ServerAsync))
            msg = 0;
        }

		assert(msg);
        if (!msg)
          continue;

		// process request
		local_runs = 0;
		idle_runs.store(0, std::memory_order_relaxed);
		void *req = read_request(msg->payload); // must be fast!

        assert(req);
        if (!req && !terminated) // comm error
        {
          IPCControl::State s = msg->control.state.exchange(IPCControl::CommErr, std::memory_order_acq_rel);
          assert(s == IPCControl::ServerRecv || s == IPCControl::ServerAsync || terminated);
          terminate = true;
          continue;
        }

        if (async && !terminated)
        {
          IPCControl::State s = msg->control.state.exchange(IPCControl::NotUsed, std::memory_order_acq_rel);
          assert(s == IPCControl::ServerAsync || terminated);
          msg = 0;
          process_async_request(req);  // do stuff
        }
		else if (!terminated)
        {
          // sync request
		  void *resp = process_request(req);  // do stuff
          assert(resp);  // TODO: need to handle this...
          if (!terminated)
            send_response(resp, msg->payload); // must be fast!
		  if (!terminated)
		  {
		    IPCControl::State s = msg->control.state.exchange(IPCControl::ServerDone, std::memory_order_acq_rel);
		    assert(s == IPCControl::ServerRecv || terminated);
		    Sem_post(msg->control.ready);
		    msg = 0;
	  	  }
        }
	  }
	  
      if (!terminated)
		msg = 0;

	  completed = terminated = true;
	}
  };
  
  typedef boost::interprocess::shared_memory_object shmem_device;
  typedef boost::interprocess::mapped_region shared_memory;
  
  IPCChannel *channel;
  std::string device_name;

  Service(time_t exp = 0, int max_thr = IPC_CHANNEL_MESSAGES) : max_threads(max_thr), done (false), expire_at(exp) 
  {
    static int dc = 0;
    std::stringstream s;
	s << "shmem_" << dc++;
	device_name = s.str();
	shmem_device::remove(device_name.c_str());
	device = new shmem_device(boost::interprocess::create_only, device_name.c_str(), boost::interprocess::read_write);
	device->truncate(IPC_MESSAGE_SIZE*IPC_CHANNEL_MESSAGES);
	shmem = new shared_memory(*device, boost::interprocess::read_write);
	channel = (IPCChannel *)shmem->get_address();
	std::cout << "launching service " << device_name.c_str() << "\n";
	// init semaphores
	channel->service_proc = GetCurrentProcessId();
	channel->stuff_to_do = Sem_create();
	for (int i = 0; i < IPC_CHANNEL_MESSAGES; i++)
	  channel->msgs[i].control.ready = Sem_create();
  }
  
  bool Run()  // return false when expires
  {
    assert(!done);
	int used_count = 0;
	int now = time(0);
	bool expire = expire_at && expire_at < now;
    if (threads.empty())  // launch primary service thread
	  threads.push_back(new Thread(this, true));
	else
	{ // running service maintenance
	  for (int i = 0; i < IPC_CHANNEL_MESSAGES; i++)
	    if (channel->msgs[i].control.state != IPCControl::NotUsed)
		  used_count++;
      CleanupThreads();
	  if (threads.size() < max_threads && threads.size() < used_count+1) // start another thread
	    threads.push_back(new Thread(this));
	  if (used_count && expire_at && expire_at < now + 5)
	  { // don't expire active service
	    expire = false;
		expire_at = now + 5;
	  }
	  time_to_wait(-1);
	}
	return !expire;
  }
  
  bool Shutdown()
  {
    // ask nicely
    for (auto i = threads.begin(); i != threads.end(); i++)
	  (*i)->terminate = true;
	bool ret = CleanupThreads();
	// kill threads
	for (auto i = threads.begin(); i != threads.end(); i++)
	  (*i)->terminated = true;
	ret |= CleanupThreads();
	// cleanup channel
	bool again;
	do
	{
	  again = false;
	  for (int i = 0; i < IPC_CHANNEL_MESSAGES; i++)
	  {
	    IPCControl::State s = IPCControl::NotUsed;
		if (channel->msgs[i].control.state.compare_exchange_strong(s, IPCControl::ServerDown) || s == IPCControl::ServerDown)
		  continue;
		if (s == IPCControl::ClientSend || s == IPCControl::ServerDone)
		  again = true;
		else  // shouldn't be
		  channel->msgs[i].control.state = IPCControl::ServerDown;
	  }
	  if (again)  // wait for client
	    std::this_thread::yield();
	} 
	while (again);
	// cleanup semaphores
	for (int i = 0; i < IPC_CHANNEL_MESSAGES; i++)
	  Sem_destroy(channel->msgs[i].control.ready);
	Sem_destroy(channel->stuff_to_do);
	// destroy shared memory
	delete shmem;
	delete device;
	shmem_device::remove(device_name.c_str());
	done = true;
	std::cout << "shutdown service " << device_name.c_str() << "\n";
	return ret;
  }
  
private:
  int max_threads;
  std::vector<Thread *> threads;
  shmem_device *device;
  shared_memory *shmem;
  bool done;
  time_t expire_at;
  
  bool CleanupThreads()
  {
    int count = 0;
	do 
	{
	  bool to_wait = false;
	  for (auto i = threads.begin(); i != threads.end(); i++)
	    to_wait |= (*i)->terminate && !(*i)->completed;
      if (!to_wait)
	    break;
	  std::this_thread::sleep_for(std::chrono::microseconds(10));
	}
	while (++count < 1000);
	int cleanedup = 0;
    for (int i = threads.size() - 1; i >= 0; i--)
	{
	  Thread *th = threads[i];
	  if (th->terminated)
	  {
	    if (th->msg) // signal error to client
		{
	      IPCControl::State s = IPCControl::ServerRecv;
		  th->msg->control.state.compare_exchange_strong(s, IPCControl::ServerErr);
		}
	    threads.erase(threads.begin() + i);
		delete th;
		cleanedup++;
	  }
	}
	return cleanedup > 0;
  }
};
