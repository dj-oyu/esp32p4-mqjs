#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ui_status.h"
#include "ui_tab5.h"

static ui_status_t s_st;
static SemaphoreHandle_t s_mtx;

static void push_locked(void (*mut)(void *), void *arg)
{
    if (!s_mtx) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        /* benign race at init: worst case one extra mutex is leaked */
        if (m && s_mtx == NULL)
            s_mtx = m;
        else if (m)
            vSemaphoreDelete(m);
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    mut(arg);
    ui_tab5_set_status(&s_st);
    xSemaphoreGive(s_mtx);
}

struct net_arg { bool up; const char *ip; };
static void mut_net(void *p)
{
    struct net_arg *a = p;
    s_st.wifi_up = a->up;
    strlcpy(s_st.ip, a->ip ? a->ip : "", sizeof s_st.ip);
}
void ui_status_set_net(bool wifi_up, const char *ip)
{
    struct net_arg a = { wifi_up, ip };
    push_locked(mut_net, &a);
}

static void mut_mqtt(void *p) { s_st.mqtt_up = *(bool *)p; }
void ui_status_set_mqtt(bool up)
{
    push_locked(mut_mqtt, &up);
}

struct task_arg { const char *name, *origin; };
static void mut_task(void *p)
{
    struct task_arg *a = p;
    strlcpy(s_st.task_name, a->name ? a->name : "", sizeof s_st.task_name);
    strlcpy(s_st.task_origin, a->origin ? a->origin : "",
            sizeof s_st.task_origin);
}
void ui_status_set_task(const char *name, const char *origin)
{
    struct task_arg a = { name, origin };
    push_locked(mut_task, &a);
}

static void mut_event(void *p)
{
    strlcpy(s_st.last_event, (const char *)p, sizeof s_st.last_event);
}
void ui_status_set_event(const char *event)
{
    push_locked(mut_event, (void *)event);
}
