
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

// Boost stuff
#define BOOST_DATE_TIME_NO_LIB
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
typedef boost::interprocess::shared_memory_object shmem_device;
typedef boost::interprocess::mapped_region shared_memory;

#include "shmipc.h"

// semaphores

#if defined(_WIN32)

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
      BOOL ok = DuplicateHandle(proc, other.sem, GetCurrentProcess(), &sem, 0, 0, DUPLICATE_SAME_ACCESS);
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

#elif defined(__MACH__)
#error "Not implemeted for Mac yet"
#elif defined(__unix__)

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
    sem = (sem_t *) &other.holder;
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
    NotUsed, ClientSend,                 // sync & async requests
    ClientWait, ServerRecv, ServerDone,  // sync request
    ClientAsync, ServerAsync,            // async request
    CommErr };
  std::atomic<State> state;
  IPCSem ready; 
};

struct IPCMessage
{
  IPCControl control;
  // need alignment
};

#define IPC_SYNC_PAYLOAD_SIZE (1024*64)
struct IPCSyncMessage : public IPCMessage
{
  char payload[IPC_SYNC_PAYLOAD_SIZE];
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
  IPCSem stuff_to_do;
  // need alignment
  std::atomic<bool> service_started;
  // need alignment
  IPCSyncMessage s_msgs[IPC_CHANNEL_SYNC_MESSAGES];
  IPCAsyncMessage a_msgs[IPC_CHANNEL_ASYNC_MESSAGES];
};

bool time_to_wait(int64_t count)
{
  return count > 1;
}

// service
struct shmipc_service
{
  struct Thread
  {
    std::atomic<bool> terminate;  // service is asking thread to exit
	std::atomic<bool> terminated; // service has terminated a thread
	std::atomic<bool> completed;  // thread has finished running
	std::atomic<int>  idle_runs;
    std::thread *thr;
	shmipc_service *service;
	IPCMessage *msg;
	bool primary;
    Thread(shmipc_service *s, bool p = false) : 
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
		  got_something = service->stuff_to_do->Wait(1000);
		  if (!got_something)
		  {
		    idle_runs++;
		    if (!primary && IsIdle())
		      terminate = true;
		  }
		}
		else
		  got_something = service->stuff_to_do->Wait(0);
		
		if (!got_something)  // no pending request
		{
		  std::this_thread::yield();
		  continue;
		}
		
		// acquire the request
		int s_msg_num = -1;
		for (int i = 0; i < IPC_CHANNEL_SYNC_MESSAGES && !msg; i++)
		{
		  msg = service->channel->s_msgs + i;
		  IPCControl::State s = IPCControl::ClientWait;
		  if (!msg->control.state.compare_exchange_strong(s, IPCControl::ServerRecv))
		    msg = 0;
          else
            s_msg_num = i;  // remember message number
		}
		
        for (int i = 0; i < IPC_CHANNEL_ASYNC_MESSAGES && !msg; i++)
        {
          msg = service->channel->a_msgs + i;
          IPCControl::State s = IPCControl::ClientAsync;
          if (!msg->control.state.compare_exchange_strong(s, IPCControl::ServerAsync))
            msg = 0;
          else
            async = true;
        }

		assert(msg);
        if (!msg)
          continue;

		// process request
		local_runs = 0;
		idle_runs.store(0, std::memory_order_relaxed);
        void *payload = ((IPCSyncMessage *)msg)->payload;  // same for both sync & async
		void *req = service->read_request(payload); // must be fast!

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
          service->process_async_request(req);  // do stuff
        }
		else if (!terminated)
        {
          // sync request
		  void *resp = service->process_request(req);  // do stuff
          assert(resp);  // TODO: need to handle this...
          if (!terminated)
            service->send_response(resp, payload, IPC_SYNC_PAYLOAD_SIZE); // must be fast! TODO: handle errors
		  if (!terminated)
		  {
		    IPCControl::State s = msg->control.state.exchange(IPCControl::ServerDone, std::memory_order_acq_rel);
		    assert(s == IPCControl::ServerRecv || terminated);
            service->ready[s_msg_num]->Post();
		    msg = 0;
	  	  }
        }
	  }
	  
      if (!terminated)
		msg = 0;

	  completed = terminated = true;
	}
  };
  
  IPCChannel *channel;
  std::string device_name;
  encode_fn send_response;
  decode_fn read_request;
  process_fn process_request;
  process_async_fn process_async_request;

  shmipc_service(const char *path, encode_fn enf, decode_fn decf, process_fn procf, process_async_fn aprocf) : 
    device_name(path), send_response(enf), read_request(decf), process_request(procf), process_async_request(aprocf),
    max_threads(IPC_CHANNEL_SYNC_MESSAGES+1), done(false), stuff_to_do(0), ready(0)
  {
    device = new shmem_device(boost::interprocess::open_only, path, boost::interprocess::read_write);
    shmem = new shared_memory(*device, boost::interprocess::read_write);
    channel = (IPCChannel *)shmem->get_address();
  }

  bool Run()  // return false when expires
  {
    assert(!done);
    bool has_errors = false;
    if (threads.empty())  // initialize semaphores & launch primary service thread
    {
      stuff_to_do = new IPCSem(channel->stuff_to_do);
      ready = new IPCSem*[IPC_CHANNEL_SYNC_MESSAGES];
      for (int i = 0; i < IPC_CHANNEL_SYNC_MESSAGES; i++)
        ready[i] = new IPCSem(channel->s_msgs[i].control.ready);
      channel->service_started = true;
	  threads.push_back(new Thread(this, true));
    }
	else
	{ // running service maintenance
	  int used_count = 0;
	  for (int i = 0; i < IPC_CHANNEL_SYNC_MESSAGES && !has_errors; i++)
      {
        IPCControl::State s = channel->s_msgs[i].control.state.load(std::memory_order_relaxed);
        has_errors = (s == IPCControl::CommErr);
	    if (!has_errors && s != IPCControl::NotUsed)
		  used_count++;
      }
	  for (int i = 0; i < IPC_CHANNEL_ASYNC_MESSAGES && !has_errors; i++)
      {
        IPCControl::State s = channel->a_msgs[i].control.state.load(std::memory_order_relaxed);
        has_errors = (s == IPCControl::CommErr);
	    if (!has_errors && s != IPCControl::NotUsed)
		  used_count++;
      }
      CleanupThreads();
	  if (!has_errors && threads.size() < max_threads && threads.size() < used_count+1) // start another thread
	    threads.push_back(new Thread(this));
	}
	return !has_errors;
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
	// cleanup comm channels
	for (int i = 0; i < IPC_CHANNEL_SYNC_MESSAGES; i++)
    {
      IPCControl::State s = channel->s_msgs[i].control.state.exchange(IPCControl::CommErr);
      if (s == IPCControl::ClientWait)
        ready[i]->Post();  // release client
    }
    for (int i = 0; i < IPC_CHANNEL_ASYNC_MESSAGES; i++)
      channel->a_msgs[i].control.state.store(IPCControl::CommErr);
	// cleanup semaphores
	for (int i = 0; i < IPC_CHANNEL_SYNC_MESSAGES; i++)
	  delete ready[i];
    delete stuff_to_do;
	// destroy shared memory
	delete shmem;
	delete device;

	done = true;

	return ret;
  }

  ~shmipc_service() { if (!done) Shutdown(); }
  
private:
  int max_threads;
  std::vector<Thread *> threads;
  shmem_device *device;
  shared_memory *shmem;
  bool done;
  IPCSem *stuff_to_do;
  IPCSem **ready;
  
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
	  std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	while (++count < 1000);
	int cleanedup = 0;
    for (int i = threads.size() - 1; i >= 0; i--)
	{
	  Thread *th = threads[i];
	  if (th->terminated)
	  {
	    threads.erase(threads.begin() + i);
		delete th;
		cleanedup++;
	  }
	}
	return cleanedup > 0;
  }
};

shmipc_service_t shmipc_init(char *filepath, encode_fn enf, decode_fn decf, process_fn procf, process_async_fn aprocf)
{
  shmipc_service_t service;
  try {
    service = new shmipc_service(filepath, enf, decf, procf, aprocf);
  } catch (...) { service = 0; errno = EBADF; }   // for now...
  return service;
}

int shmipc_run(shmipc_service_t ipc)
{
  int ret = ipc->Run() ? 0 : -1;
  if (ret)
    errno = ECONNABORTED;
  return ret;
}

int shmipc_shutdown(shmipc_service_t ipc)
{
  int ret = 0;
  try {
    delete ipc;
  } catch (...) { ret = -1; errno = EBADF; }  // not sure...
  return ret;
}

struct shmipc_client
{
  IPCChannel *channel;
  std::string device_name;
  encode_fn send_request;
  decode_fn read_response;

  shmipc_client(const char *path, encode_fn enc, decode_fn dec) : 
    device_name(path), send_request(enc), read_response(dec), done(false), commerr(false)
  {
    // allocate shared memory channel
    shmem_device::remove(device_name.c_str());
    device = new shmem_device(boost::interprocess::create_only, device_name.c_str(), boost::interprocess::read_write);
	device->truncate(sizeof(IPCChannel));
	shmem = new shared_memory(*device, boost::interprocess::read_write);
	channel = (IPCChannel *)shmem->get_address();
    // initialize channel
    memset(&channel->stuff_to_do, 0, sizeof(IPCSem));
    channel->stuff_to_do.Init();
    channel->service_started = false;
    for (int i = 0; i < IPC_CHANNEL_SYNC_MESSAGES; i++)
    {
      memset(&channel->s_msgs[i].control, 0, sizeof(IPCControl));
      channel->s_msgs[i].control.ready.Init();
    }
    for (int i = 0; i < IPC_CHANNEL_ASYNC_MESSAGES; i++)
      memset(&channel->a_msgs[i].control, 0, sizeof(IPCControl));
  }

  ~shmipc_client() { if (!done) Destroy(); }

  bool Destroy()
  {
    channel->s_msgs[0].control.state = IPCControl::CommErr; // signal comm shutdown to service

	delete shmem;
	delete device;
	shmem_device::remove(device_name.c_str());
    done = true;
    return true;
  }

  enum Error { NoError, DeadChannel, ProtocolError, ServerError, BadMsg };

  Error AsyncSend(void *message)
  {
    return commerr.load(std::memory_order_relaxed) ? DeadChannel : service_call(message, 0, true);
  }

  Error Request(void *req, void *resp)
  {
    assert(resp);
    return commerr.load(std::memory_order_relaxed) ? DeadChannel : service_call(req, resp, false);
  }

private:
  shmem_device *device;
  shared_memory *shmem;
  bool done;
  std::atomic<bool> commerr;

  Error service_call(void *req, void *resp, bool async)
  {
    IPCMessage *msg = 0;
    IPCControl::State s;
    int ch = 0;

    while (!channel->service_started)
      std::this_thread::yield();  // wait for service

    while (true)
    {
      if (async)
        msg = channel->a_msgs + ch;
      else
        msg = channel->s_msgs + ch;
	  s = IPCControl::NotUsed;
      if (msg->control.state.compare_exchange_strong(s, IPCControl::ClientSend))
        break;   // got comm channel

      if (s == IPCControl::CommErr)
      {
        commerr = true;
        return DeadChannel;
      }

      ch++;
      if ((async && ch == IPC_CHANNEL_ASYNC_MESSAGES) ||
         (!async && ch == IPC_CHANNEL_SYNC_MESSAGES))
      { // all messages are used by comm
        ch = 0;
        std::this_thread::yield();
        // can execute user call back here
      }
    }
  
    // send request
    void *payload = ((IPCSyncMessage *)msg)->payload;
    send_request(req, payload, async ? IPC_ASYNC_PAYLOAD_SIZE : IPC_SYNC_PAYLOAD_SIZE);  // TODO: handle errors
    s = msg->control.state.exchange(async ? IPCControl::ClientAsync : IPCControl::ClientWait, std::memory_order_acq_rel);
    assert(s == IPCControl::ClientSend);

    // signal server
    channel->stuff_to_do.Post();

    if (async)  // we are done for async
      return NoError;
  
    // wait for response
    msg->control.ready.Wait();  // TODO: handle idle
  
    s = msg->control.state.load(std::memory_order_acquire);
  
    // handle comm errors
    if (s == IPCControl::CommErr)
    {   
      commerr = true;
      return ServerError;
    }
    assert(s == IPCControl::ServerDone);
  
    // read response
    *(void **)resp = read_response(payload);
  
    // free comm channel
    s = msg->control.state.exchange(IPCControl::NotUsed, std::memory_order_relaxed);
    assert(s == IPCControl::ServerDone);
  
    return NoError;
  }
};

shmipc_client_t shmipc_dial(char *filepath, encode_fn encoder, decode_fn decoder)
{
  shmipc_client_t ipc;
  try {
    ipc = new shmipc_client(filepath, encoder, decoder);
  } catch (...) { ipc = 0; errno = EBADF; }
  return ipc;
}

static int clienterr_to_errno(shmipc_client::Error err)
{
  switch (err)
  {
    case shmipc_client::NoError: return 0;
    case shmipc_client::DeadChannel: return ECONNABORTED;
    case shmipc_client::ProtocolError: return EPROTO;
    case shmipc_client::BadMsg: return EBADMSG;
    case shmipc_client::ServerError: return EIO;
  }
  assert(0);
  return 0;
}

void *shmipc_call(shmipc_client_t ipc, void *request_object)
{
  void *ret = 0;
  int err = clienterr_to_errno(ipc->Request(request_object, &ret));
  if (err)
    errno = err;
  return err ? 0 : ret;
}

int shmipc_send_async(shmipc_client_t ipc, void *message)
{
  int err = clienterr_to_errno(ipc->AsyncSend(message));
  if (err)
    errno = err;
  return err ? -1 : 0;
}

int shmipc_close(shmipc_client_t ipc)
{
  int ret = 0;
  try {
    delete ipc;
  } catch (...) { ret = -1; errno = EBADF; }
  return ret;
}

