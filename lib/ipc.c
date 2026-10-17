#include "ipc.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/un.h>
#include <fcntl.h>

#define SD_LISTEN_FDS_START 3


static int get_systemd_socket(void) {
    const char *pid_s = getenv("LISTEN_PID");
    const char *fds_s = getenv("LISTEN_FDS");
    if (!pid_s || !fds_s) return -1;

    char *end;
    long pid = strtol(pid_s, &end, 10);
    if (*end || pid != (long)getpid()) return -1;   /* not meant for us */

    long n = strtol(fds_s, &end, 10);
    if (*end || n < 1) return -1;

    int fd = SD_LISTEN_FDS_START;

    /* sanity check: must be a listening unix stream socket */
    int accepting = 0;
    socklen_t len = sizeof(accepting);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &len) < 0 || !accepting)
        return -1;

    struct sockaddr_un addr;
    len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0 || addr.sun_family != AF_UNIX)
        return -1;

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    unsetenv("LISTEN_PID");
    unsetenv("LISTEN_FDS");
    unsetenv("LISTEN_FDNAMES");
    return fd;
}

static int make_server_socket(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("ipc: socket"); return -1; }

    unlink(path);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("ipc: bind"); close(fd); return -1;
    }
    if (listen(fd, IPC_BACKLOG) < 0) {
        perror("ipc: listen"); close(fd); return -1;
    }
    return fd;
}

static void *listener_thread(void *arg) {
    ipc_ctx_t *ctx = (ipc_ctx_t *)arg;

    while (ctx->running) {
        struct sockaddr_un peer;
        socklen_t peer_len = sizeof(peer);

        int client_fd = accept(ctx->server_fd,
                               (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINVAL || errno == EBADF) break; /* shutting down */
            if (errno == EINTR)                   continue;
            perror("ipc: accept");
            continue;
        }

        printf("[ipc] new client fd=%d\n", client_fd);

        ipc_client_arg_t *carg = malloc(sizeof(ipc_client_arg_t));
        if (!carg) { close(client_fd); continue; }
        carg->client_fd = client_fd;
        carg->user_data = ctx->user_data;

        pthread_t tid;
        if (pthread_create(&tid, NULL,
                           (void *(*)(void *))ctx->handler, carg) != 0) {
            perror("ipc: pthread_create");
            free(carg);
            close(client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    printf("[ipc] listener exiting\n");
    return NULL;
}

int create_ipc(ipc_ctx_t *ctx, ipc_client_fn handler, const char *socket_path, void *user_data) {
    ctx->handler = handler;
    ctx->user_data = user_data;
    ctx->running = 1;
    ctx->socket_path = strdup(socket_path);

    int fd = get_systemd_socket();
    if (fd >= 0) {
        ctx->server_fd = fd;
        ctx->socket_activated = 1;
        printf("[ipc] using socket inherited from systemd\n");
    } else {
        ctx->socket_activated = 0;
        ctx->server_fd = make_server_socket(socket_path);
        if (ctx->server_fd < 0) return -1;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, listener_thread, ctx) != 0) {
        perror("ipc: pthread_create");
        close(ctx->server_fd);
        if (!ctx->socket_activated) unlink(socket_path);
        return -1;
    }
    ctx->_tid = tid;

    if (!ctx->socket_activated) printf("[ipc] listening on %s\n", socket_path);
    return 0;
}

void destroy_ipc(ipc_ctx_t *ctx) {
    ctx->running = 0;
    shutdown(ctx->server_fd, SHUT_RDWR);
    close(ctx->server_fd);
    if (!ctx->socket_activated) unlink(ctx->socket_path);  /* systemd owns it otherwise */
    pthread_join(ctx->_tid, NULL);
    free(ctx->socket_path);
    printf("[ipc] shut down\n");
}