/*
 * SSH client session task (wolfSSH). One session at a time. Owns the
 * TCP socket and the WOLFSSH object exclusively; the only cross-task
 * surfaces are:
 *   - a tx StreamBuffer (JS keystrokes -> session task)
 *   - mqjs_post_ssh_data() (server bytes -> JS event loop, heap copies)
 *   - a few volatile flags (resize request, stop request, state)
 *
 * Connection flow (blocking, on the session task only): resolve host ->
 * TCP connect -> wolfSSH handshake + password auth -> pty-req + shell
 * (WOLFSSH_SESSION_TERMINAL). After that the task loops with a short
 * socket recv timeout so it can interleave reads with draining the tx
 * buffer and applying pty resizes.
 *
 * Host-key policy v1: accept any (the fingerprint is logged). Password
 * auth only; public-key auth is a later phase.
 */
#include "sdkconfig.h"
#if CONFIG_MQJS_SSH

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* wolfssh/ssh.h keys off WOLFSSL_USER_SETTINGS (the wolfSSL macro) to
   pull settings.h -> user_settings.h instead of a generated options.h */
#define WOLFSSL_USER_SETTINGS
#define WOLFSSH_USER_SETTINGS
#include <wolfssh/ssh.h>
#include <wolfssh/error.h>

#include "sshc.h"

/* mqjs sinks (extern decl instead of REQUIRES mqjs: mqjs already
   depends on this component for sshc.h, so a REQUIRES back would be a
   cycle — same trick as ui_tab5.cpp / wifi.c) */
extern bool mqjs_post_ssh_data(char *data, size_t len);
extern void mqjs_post_ssh_closed(const char *reason);

static const char *TAG = "sshc";

#define SSH_TX_BUF_BYTES  2048
#define SSH_RX_CHUNK      1024
#define SSH_TASK_STACK    8192
#define SSH_TASK_PRIO     5
#define SSH_CONNECT_TMO_S 10
#define SSH_RECV_TMO_MS   50

/* connection parameters, filled by mqjs_ssh_connect before the task runs */
typedef struct {
    char host[64];
    char user[32];
    char pass[64];
    int  port;
    int  cols, rows;
} ssh_params_t;

static TaskHandle_t      s_task;
static StreamBufferHandle_t s_tx;     /* JS -> session bytes */
static ssh_params_t      s_params;
static volatile bool     s_stop;      /* close() request */
static volatile bool     s_up;        /* shell channel established */
static volatile bool     s_active;    /* task alive (connecting or up) */
static volatile int      s_resize_cols, s_resize_rows;
static volatile bool     s_resize_req;

/* wolfSSH password auth callback: hand back s_params.pass for a
   PASSWORD request, refuse everything else (no pubkey/keyboard yet). */
static int ssh_userauth_cb(byte authType, WS_UserAuthData *authData, void *ctx)
{
    (void)ctx;
    if (authType == WOLFSSH_USERAUTH_PASSWORD) {
        authData->sf.password.password = (const byte *)s_params.pass;
        authData->sf.password.passwordSz = (word32)strlen(s_params.pass);
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    return WOLFSSH_USERAUTH_FAILURE;
}

/* host-key check: accept any (v1). The fingerprint is logged. */
static int ssh_pubkey_check_cb(const byte *pubKey, word32 pubKeySz, void *ctx)
{
    (void)pubKey;
    (void)ctx;
    ESP_LOGW(TAG, "accepting server host key (%u bytes) without verification",
             (unsigned)pubKeySz);
    return 0;
}

/* blocking TCP connect with a bounded timeout; returns fd or -1 */
static int tcp_connect(const char *host, int port)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", host);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    struct timeval tv = { .tv_sec = SSH_CONNECT_TMO_S, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        ESP_LOGE(TAG, "TCP connect to %s:%d failed", host, port);
        close(fd);
        return -1;
    }
    return fd;
}

/* the session: connect, handshake, then read/write loop. Always posts
   exactly one mqjs_post_ssh_closed() before returning. */
static void ssh_session(WOLFSSH_CTX *ctx)
{
    const char *reason = "closed";
    WOLFSSH *ssh = NULL;
    int fd = -1;

    fd = tcp_connect(s_params.host, s_params.port);
    if (fd < 0) {
        reason = "connect failed";
        goto done;
    }

    ssh = wolfSSH_new(ctx);
    if (!ssh) {
        reason = "no mem";
        goto done;
    }
    wolfSSH_SetUserAuthCtx(ssh, NULL);
    wolfSSH_SetPublicKeyCheckCtx(ssh, NULL);
    if (wolfSSH_SetUsername(ssh, s_params.user) != WS_SUCCESS) {
        reason = "bad username";
        goto done;
    }
    /* pseudo-terminal + shell */
    if (wolfSSH_SetChannelType(ssh, WOLFSSH_SESSION_TERMINAL, NULL, 0)
        != WS_SUCCESS) {
        reason = "no pty";
        goto done;
    }
    wolfSSH_set_fd(ssh, fd);

    /* handshake + auth + channel open (blocking; socket has a 10s tmo) */
    int rc;
    do {
        rc = wolfSSH_connect(ssh);
    } while (rc != WS_SUCCESS &&
             (wolfSSH_get_error(ssh) == WS_WANT_READ ||
              wolfSSH_get_error(ssh) == WS_WANT_WRITE) && !s_stop);
    if (rc != WS_SUCCESS) {
        ESP_LOGE(TAG, "wolfSSH_connect: %s", wolfSSH_get_error_name(ssh));
        reason = "auth/handshake failed";
        goto done;
    }

    /* wolfSSH's pty-req sends a fixed 80x24 on a no-filesystem embedded
       build (GetTerminalInfo has no termios to query), and live resize
       (wolfSSH_ChangeTerminalSize) is only compiled with a filesystem.
       So the requested cols/rows are advisory on this config — the JS
       terminal should assume 80x24. Apply the size only where the lib
       actually provides the call. */
#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)
    if (s_params.cols > 0 && s_params.rows > 0)
        wolfSSH_ChangeTerminalSize(ssh, s_params.cols, s_params.rows, 0, 0);
#endif

    s_up = true;
    ESP_LOGI(TAG, "shell up on %s@%s:%d", s_params.user, s_params.host,
             s_params.port);

    /* shorter recv timeout for the interactive loop so tx/resize get
       serviced promptly between reads */
    struct timeval tv = { .tv_sec = 0, .tv_usec = SSH_RECV_TMO_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    byte rxbuf[SSH_RX_CHUNK];
    while (!s_stop) {
        /* pending pty resize (only where the lib compiled the call) */
        if (s_resize_req) {
            s_resize_req = false;
#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)
            wolfSSH_ChangeTerminalSize(ssh, s_resize_cols, s_resize_rows, 0, 0);
#endif
        }

        /* drain queued keystrokes to the server */
        byte txbuf[256];
        size_t n = xStreamBufferReceive(s_tx, txbuf, sizeof txbuf, 0);
        while (n > 0) {
            int sent = wolfSSH_stream_send(ssh, txbuf, (word32)n);
            if (sent <= 0) {
                int e = wolfSSH_get_error(ssh);
                if (e == WS_WANT_READ || e == WS_WANT_WRITE)
                    break; /* retry next loop */
                reason = "write error";
                goto done;
            }
            n = xStreamBufferReceive(s_tx, txbuf, sizeof txbuf, 0);
        }

        /* read server output (blocks up to SSH_RECV_TMO_MS) */
        int got = wolfSSH_stream_read(ssh, rxbuf, sizeof rxbuf);
        if (got > 0) {
            char *copy = malloc(got);
            if (copy) {
                memcpy(copy, rxbuf, got);
                if (!mqjs_post_ssh_data(copy, got)) {
                    free(copy); /* JS loop wedged: give up the session */
                    reason = "rx overflow";
                    goto done;
                }
            }
        } else if (got == WS_EOF) {
            reason = "remote closed";
            goto done;
        } else if (got != WS_WANT_READ && got != WS_WANT_WRITE) {
            int e = wolfSSH_get_error(ssh);
            if (e != WS_WANT_READ && e != WS_WANT_WRITE) {
                ESP_LOGE(TAG, "stream_read: %s", wolfSSH_get_error_name(ssh));
                reason = "read error";
                goto done;
            }
        }
    }
    if (s_stop)
        reason = "closed";

done:
    if (ssh) {
        wolfSSH_stream_exit(ssh, 0);
        wolfSSH_free(ssh);
    }
    if (fd >= 0)
        close(fd);
    s_up = false;
    mqjs_post_ssh_closed(reason);
}

static void ssh_task(void *arg)
{
    (void)arg;
    WOLFSSH_CTX *ctx = NULL;
    if (wolfSSH_Init() != WS_SUCCESS) {
        mqjs_post_ssh_closed("wolfSSH init failed");
        goto cleanup;
    }
    ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (!ctx) {
        mqjs_post_ssh_closed("no ctx");
        goto cleanup;
    }
    wolfSSH_SetUserAuth(ctx, ssh_userauth_cb);
    wolfSSH_CTX_SetPublicKeyCheck(ctx, ssh_pubkey_check_cb);

    ssh_session(ctx);

cleanup:
    if (ctx)
        wolfSSH_CTX_free(ctx);
    wolfSSH_Cleanup();
    s_active = false;
    s_up = false;
    s_task = NULL;
    vTaskDelete(NULL);
}

bool mqjs_ssh_connect(const char *host, int port, const char *user,
                      const char *pass, int cols, int rows)
{
    if (s_active)
        return false;
    if (!host || !user || !pass)
        return false;

    memset(&s_params, 0, sizeof s_params);
    strlcpy(s_params.host, host, sizeof s_params.host);
    strlcpy(s_params.user, user, sizeof s_params.user);
    strlcpy(s_params.pass, pass, sizeof s_params.pass);
    s_params.port = (port > 0) ? port : 22;
    s_params.cols = cols;
    s_params.rows = rows;

    if (!s_tx) {
        s_tx = xStreamBufferCreate(SSH_TX_BUF_BYTES, 1);
        if (!s_tx)
            return false;
    } else {
        xStreamBufferReset(s_tx);
    }

    s_stop = false;
    s_up = false;
    s_resize_req = false;
    s_active = true;
    if (xTaskCreate(ssh_task, "ssh", SSH_TASK_STACK, NULL, SSH_TASK_PRIO,
                    &s_task) != pdPASS) {
        s_active = false;
        return false;
    }
    return true;
}

bool mqjs_ssh_write(const void *data, size_t len)
{
    if (!s_tx || !s_up || !data || !len)
        return false;
    /* non-blocking: a full tx buffer means the link is saturated */
    return xStreamBufferSend(s_tx, data, len, 0) == len;
}

void mqjs_ssh_resize(int cols, int rows)
{
    if (cols <= 0 || rows <= 0)
        return;
    s_resize_cols = cols;
    s_resize_rows = rows;
    s_resize_req = true;
}

void mqjs_ssh_close(void)
{
    if (!s_active)
        return;
    s_stop = true;
    /* wait (bounded) for the task to self-delete; recv timeout is 50ms
       in the loop, connect timeout up to 10s in the worst case */
    for (int i = 0; i < 300 && s_active; i++)
        vTaskDelay(pdMS_TO_TICKS(50));
    if (s_active)
        ESP_LOGW(TAG, "ssh task did not stop in time");
}

bool mqjs_ssh_active(void) { return s_active; }
bool mqjs_ssh_up(void) { return s_up; }

#endif /* CONFIG_MQJS_SSH */
