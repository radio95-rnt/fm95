#pragma once

#include <stdint.h>
#include <pthread.h>

#define IPC_SOCKET_PATH "/tmp/ipc_demo.sock"
#define IPC_BACKLOG 1

typedef void *(*ipc_client_fn)(void *client_fd_ptr);

typedef struct {
    int server_fd;
    volatile int running;
    ipc_client_fn handler;
    pthread_t _tid;       /* internal — do not touch */
    const char* socket_path;
} ipc_ctx_t;
int create_ipc(ipc_ctx_t *ctx, ipc_client_fn handler, const char* socket_path);
void destroy_ipc(ipc_ctx_t *ctx);