/*
 * Target App Manager contract.
 *
 * This header defines the destination of the slot-to-App/Worker migration
 * documented in docs/app-manager-migration.md. It is intentionally not wired
 * into mqjs_runtime.c yet.
 *
 * App Manager state has one writer: the js_task. Functions named request_*
 * enqueue copied requests and may be called from other tasks. Internal Worker
 * indexes and generations must never be exposed through this API.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQJS_APP_NAME_MAX 32

typedef uint32_t mqjs_app_id_t;

#define MQJS_APP_ID_INVALID ((mqjs_app_id_t)0)

typedef enum {
    MQJS_APP_STOPPED = 0,
    MQJS_APP_STARTING,
    MQJS_APP_RUNNING,
    MQJS_APP_SUSPENDED,
    MQJS_APP_STOPPING,
} mqjs_app_state_t;

typedef enum {
    MQJS_APP_VIEW_NONE = 0,
    MQJS_APP_VIEW_BACKGROUND,
    MQJS_APP_VIEW_FOREGROUND,
} mqjs_app_view_t;

typedef enum {
    MQJS_APP_KIND_APP = 0,
    MQJS_APP_KIND_SERVICE,
    MQJS_APP_KIND_SYSTEM,
} mqjs_app_kind_t;

typedef enum {
    MQJS_APP_STOP_USER = 0,
    MQJS_APP_STOP_IDLE,
    MQJS_APP_STOP_UPDATED,
    MQJS_APP_STOP_EVICTED,
    MQJS_APP_STOP_ERROR,
} mqjs_app_stop_reason_t;

enum {
    MQJS_APP_AUTOSTART       = 1u << 0,
    MQJS_APP_RESTART_ON_EXIT = 1u << 1,
    MQJS_APP_KEEP_ALIVE      = 1u << 2,
    MQJS_APP_EVICTABLE       = 1u << 3,
    MQJS_APP_HEADLESS        = 1u << 4,
    MQJS_APP_STOPPABLE       = 1u << 5,
};

typedef struct {
    uint32_t flags;
    int8_t priority;
} mqjs_app_policy_t;

/* A copied, read-only view of an App. Worker details are deliberately absent. */
typedef struct {
    mqjs_app_id_t id;
    char name[MQJS_APP_NAME_MAX];
    mqjs_app_state_t state;
    mqjs_app_view_t view;
    mqjs_app_kind_t kind;
    mqjs_app_policy_t policy;
    int64_t last_active_ms;
} mqjs_app_snapshot_t;

/*
 * Initialize and advance the manager. These mutate App/Worker state and must
 * only run on js_task.
 */
void mqjs_apps_init(void);
void mqjs_apps_update(int64_t now_ms);

/*
 * Queue lifecycle requests by stable App name. Each function copies `name`
 * before returning and is safe to call from a non-ISR task. Acceptance means
 * only that the request was queued; policy checks and transitions happen on
 * js_task.
 */
bool mqjs_apps_request_start(const char *name);
bool mqjs_apps_request_open(const char *name);
bool mqjs_apps_request_focus(const char *name);
bool mqjs_apps_request_stop(const char *name, mqjs_app_stop_reason_t reason);
bool mqjs_apps_request_suspend(const char *name);
bool mqjs_apps_request_resume(const char *name);

#ifdef __cplusplus
}
#endif
