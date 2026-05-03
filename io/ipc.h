#pragma once

#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>

#define IPC_BACKLOG 1

typedef struct {
    int  client_fd;
    void *user_data;
} ipc_client_arg_t;

typedef void *(*ipc_client_fn)(ipc_client_arg_t *arg);

typedef struct {
    int server_fd;
    volatile int running;
    ipc_client_fn handler;
    void *user_data;
    pthread_t _tid;       /* internal — do not touch */
    char *socket_path;
} ipc_ctx_t;

int  create_ipc(ipc_ctx_t *ctx, ipc_client_fn handler,
                const char *socket_path, void *user_data);
void destroy_ipc(ipc_ctx_t *ctx);