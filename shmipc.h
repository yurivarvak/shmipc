
#ifndef SHMIPC
#define SHMIPC

#ifdef __cplusplus
extern "C" {
#endif

struct shmipc_client;
struct shmipc_service;
typedef struct shmipc_client_t *shmipc_client;
typedef struct shmipc_service_t *shmipc_service;

typedef int (*encode_fn)(void *source_object, void *dest_msg, int max_msg_size)
typedef void *(*decode_fn)(void *source_msg)
typedef void *(*process_fn)(void *request_object)
typedef void (*process_async_fn)(void *request_object)

/* client */
shmipc_client_t shmipc_dial(char *filepath, encode_fn encoder, decode_fn decoder);
void *shmipc_call(shmipc_client_t ipc, void *request_object);
int shmipc_send_async(shmipc_client_t ipc, void *message);
int shmipc_close(shmipc_client_t ipc);

/* service */
shmipc_service_t shmipc_init(char *filepath, 
                             encode_fn encoder, decode_fn decoder, 
                             process_fn processor, process_async_fn async_processor);
int shmipc_run(shmipc_service_t ipc);
int shmipc_shutdown(shmipc_service_t ipc);

#ifdef __cplusplus
}
#endif

#endif  /* SHMIPC */
