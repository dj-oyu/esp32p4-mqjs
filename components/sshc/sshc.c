/*
 * SSH client session tasks (wolfSSH), W3: up to SSHC_MAX_SESSIONS
 * concurrent sessions, handle-based (design docs/widget-framework-design.md
 * §7). Each session owns its TCP socket and WOLFSSH object exclusively on
 * its own task; the only cross-task surfaces per session are:
 *   - a tx StreamBuffer (JS keystrokes -> session task)
 *   - mqjs_post_ssh_data(id, ...) (server bytes -> JS event loop, heap)
 *   - a few volatile flags (resize request, stop request, state)
 *
 * Session ids are (generation << 2 | slot) + 1-based so a stale JS id
 * never addresses a reused slot. 0 = invalid.
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
extern bool mqjs_post_ssh_data(int id, char *data, size_t len);
extern void mqjs_post_ssh_closed(int id, const char *reason);

static const char *TAG = "sshc";

#define SSH_TX_BUF_BYTES  2048
#define SSH_RX_CHUNK      1024
#define SSH_TASK_STACK    8192
#define SSH_TASK_PRIO     5
#define SSH_CONNECT_TMO_S 10
#define SSH_RECV_TMO_MS   50

typedef struct {
    char host[64];
    char user[32];
    char pass[64];
    int  port;
    int  cols, rows;
} ssh_params_t;

/* one concurrent session (~8KB task stack + wolfSSH state + crypto
   buffers of internal RAM each — hence the small cap, design §7) */
typedef struct {
    volatile bool active;  /* task alive (connecting or up) */
    volatile bool up;      /* shell channel established */
    volatile bool stop;    /* close() request */
    volatile bool resize_req;
    volatile int  resize_cols, resize_rows;
    StreamBufferHandle_t tx;
    ssh_params_t params;
    uint16_t gen;          /* bumped per connect; part of the public id */
    int id;                /* public id while active */
} ssh_sess_t;

static ssh_sess_t s_sess[SSHC_MAX_SESSIONS];

static int sess_id(int slot)
{
    return (int)((s_sess[slot].gen << 2) | (unsigned)slot) + 1;
}

/* public id -> slot, -1 when stale/invalid/inactive */
static int sess_lookup(int id)
{
    if (id <= 0)
        return -1;
    int slot = (id - 1) & 3;
    if (slot >= SSHC_MAX_SESSIONS)
        return -1;
    if (!s_sess[slot].active || s_sess[slot].id != id)
        return -1;
    return slot;
}

/* wolfSSH password auth callback; ctx = the session (set per WOLFSSH) */
static int ssh_userauth_cb(byte authType, WS_UserAuthData *authData, void *ctx)
{
    ssh_sess_t *s = ctx;
    if (authType == WOLFSSH_USERAUTH_PASSWORD && s) {
        authData->sf.password.password = (const byte *)s->params.pass;
        authData->sf.password.passwordSz = (word32)strlen(s->params.pass);
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
static void ssh_session(ssh_sess_t *s, WOLFSSH_CTX *ctx)
{
    const char *reason = "closed";
    WOLFSSH *ssh = NULL;
    int fd = -1;

    fd = tcp_connect(s->params.host, s->params.port);
    if (fd < 0) {
        reason = "connect failed";
        goto done;
    }

    ssh = wolfSSH_new(ctx);
    if (!ssh) {
        reason = "no mem";
        goto done;
    }
    wolfSSH_SetUserAuthCtx(ssh, s);
    wolfSSH_SetPublicKeyCheckCtx(ssh, NULL);
    if (wolfSSH_SetUsername(ssh, s->params.user) != WS_SUCCESS) {
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
              wolfSSH_get_error(ssh) == WS_WANT_WRITE) && !s->stop);
    if (rc != WS_SUCCESS) {
        ESP_LOGE(TAG, "wolfSSH_connect: %s", wolfSSH_get_error_name(ssh));
        reason = "auth/handshake failed";
        goto done;
    }

    /* wolfSSH's pty-req sends a fixed 80x24 (GetTerminalInfo has no local
       termios to query on the device), so immediately resize the server's
       pty to the grid the JS terminal actually uses (derived from the
       screen + cell size, e.g. 80x33). wolfSSH_ChangeTerminalSize is
       compiled in because the project undefines NO_FILESYSTEM for wolfSSH
       (see components/sshc/wolfssl_override/user_settings.h); the guard
       stays so a no-filesystem build still compiles cleanly. */
#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)
    if (s->params.cols > 0 && s->params.rows > 0)
        wolfSSH_ChangeTerminalSize(ssh, s->params.cols, s->params.rows, 0, 0);
#endif

    s->up = true;
    ESP_LOGI(TAG, "shell up on %s@%s:%d (session %d)", s->params.user,
             s->params.host, s->params.port, s->id);

    /* shorter recv timeout for the interactive loop so tx/resize get
       serviced promptly between reads */
    struct timeval tv = { .tv_sec = 0, .tv_usec = SSH_RECV_TMO_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    byte rxbuf[SSH_RX_CHUNK];
    while (!s->stop) {
        /* pending pty resize (only where the lib compiled the call) */
        if (s->resize_req) {
            s->resize_req = false;
#if defined(WOLFSSH_TERM) && !defined(NO_FILESYSTEM)
            wolfSSH_ChangeTerminalSize(ssh, s->resize_cols, s->resize_rows,
                                       0, 0);
#endif
        }

        /* drain queued keystrokes to the server */
        byte txbuf[256];
        size_t n = xStreamBufferReceive(s->tx, txbuf, sizeof txbuf, 0);
        while (n > 0) {
            int sent = wolfSSH_stream_send(ssh, txbuf, (word32)n);
            if (sent <= 0) {
                int e = wolfSSH_get_error(ssh);
                if (e == WS_WANT_READ || e == WS_WANT_WRITE)
                    break; /* retry next loop */
                reason = "write error";
                goto done;
            }
            n = xStreamBufferReceive(s->tx, txbuf, sizeof txbuf, 0);
        }

        /* read server output (blocks up to SSH_RECV_TMO_MS) */
        int got = wolfSSH_stream_read(ssh, rxbuf, sizeof rxbuf);
        if (got > 0) {
            char *copy = malloc(got);
            if (copy) {
                memcpy(copy, rxbuf, got);
                if (!mqjs_post_ssh_data(s->id, copy, got)) {
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
    if (s->stop)
        reason = "closed";

done:
    if (ssh) {
        wolfSSH_stream_exit(ssh, 0);
        wolfSSH_free(ssh);
    }
    if (fd >= 0)
        close(fd);
    s->up = false;
    mqjs_post_ssh_closed(s->id, reason);
}

/* one-time library init: wolfSSH_Init/Cleanup pairs are refcounted in
   wolfCrypt, but with several session tasks racing it is simpler to do
   it exactly once and never clean up (device runs forever anyway) */
static bool ssh_lib_init(void)
{
    static bool inited;
    if (!inited) {
        if (wolfSSH_Init() != WS_SUCCESS)
            return false;
        inited = true;
    }
    return true;
}

static void ssh_task(void *arg)
{
    ssh_sess_t *s = arg;
    WOLFSSH_CTX *ctx = NULL;
    if (!ssh_lib_init()) {
        mqjs_post_ssh_closed(s->id, "wolfSSH init failed");
        goto cleanup;
    }
    ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_CLIENT, NULL);
    if (!ctx) {
        mqjs_post_ssh_closed(s->id, "no ctx");
        goto cleanup;
    }
    wolfSSH_SetUserAuth(ctx, ssh_userauth_cb);
    wolfSSH_CTX_SetPublicKeyCheck(ctx, ssh_pubkey_check_cb);

    ssh_session(s, ctx);

cleanup:
    if (ctx)
        wolfSSH_CTX_free(ctx);
    s->up = false;
    s->active = false; /* last: frees the slot for reuse */
    vTaskDelete(NULL);
}

int mqjs_ssh_connect(const char *host, int port, const char *user,
                     const char *pass, int cols, int rows)
{
    if (!host || !user || !pass)
        return 0;
    int slot = -1;
    for (int i = 0; i < SSHC_MAX_SESSIONS; i++) {
        if (!s_sess[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return 0; /* all sessions in use */
    ssh_sess_t *s = &s_sess[slot];

    memset(&s->params, 0, sizeof s->params);
    strlcpy(s->params.host, host, sizeof s->params.host);
    strlcpy(s->params.user, user, sizeof s->params.user);
    strlcpy(s->params.pass, pass, sizeof s->params.pass);
    s->params.port = (port > 0) ? port : 22;
    s->params.cols = cols;
    s->params.rows = rows;

    if (!s->tx) {
        s->tx = xStreamBufferCreate(SSH_TX_BUF_BYTES, 1);
        if (!s->tx)
            return 0;
    } else {
        xStreamBufferReset(s->tx);
    }

    s->gen++;
    s->id = sess_id(slot);
    s->stop = false;
    s->up = false;
    s->resize_req = false;
    s->active = true;
    char name[8];
    snprintf(name, sizeof name, "ssh%d", slot);
    if (xTaskCreate(ssh_task, name, SSH_TASK_STACK, s, SSH_TASK_PRIO,
                    NULL) != pdPASS) {
        s->active = false;
        return 0;
    }
    return s->id;
}

bool mqjs_ssh_write(int id, const void *data, size_t len)
{
    int slot = sess_lookup(id);
    if (slot < 0 || !s_sess[slot].up || !data || !len)
        return false;
    /* non-blocking: a full tx buffer means the link is saturated */
    return xStreamBufferSend(s_sess[slot].tx, data, len, 0) == len;
}

void mqjs_ssh_resize(int id, int cols, int rows)
{
    int slot = sess_lookup(id);
    if (slot < 0 || cols <= 0 || rows <= 0)
        return;
    s_sess[slot].resize_cols = cols;
    s_sess[slot].resize_rows = rows;
    s_sess[slot].resize_req = true;
}

void mqjs_ssh_close(int id)
{
    int slot = sess_lookup(id);
    if (slot < 0)
        return;
    s_sess[slot].stop = true;
    /* wait (bounded) for the task to self-delete; recv timeout is 50ms
       in the loop, connect timeout up to 10s in the worst case */
    for (int i = 0; i < 300 && s_sess[slot].active; i++)
        vTaskDelay(pdMS_TO_TICKS(50));
    if (s_sess[slot].active)
        ESP_LOGW(TAG, "ssh task %d did not stop in time", id);
}

void mqjs_ssh_close_all(void)
{
    for (int i = 0; i < SSHC_MAX_SESSIONS; i++) {
        if (s_sess[i].active)
            s_sess[i].stop = true;
    }
    /* one bounded wait for all of them together */
    for (int t = 0; t < 300; t++) {
        bool any = false;
        for (int i = 0; i < SSHC_MAX_SESSIONS; i++)
            any = any || s_sess[i].active;
        if (!any)
            return;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "some ssh tasks did not stop in time");
}

bool mqjs_ssh_active(void)
{
    for (int i = 0; i < SSHC_MAX_SESSIONS; i++)
        if (s_sess[i].active)
            return true;
    return false;
}

bool mqjs_ssh_up(int id)
{
    int slot = sess_lookup(id);
    return slot >= 0 && s_sess[slot].up;
}

#endif /* CONFIG_MQJS_SSH */
