/* Host unit test for the App record table (app-manager Phase 2).
 *
 *   gcc -O2 -I.. -o /tmp/amgr_test app_manager_test.c \
 *       ../app/mqjs_app_manager.c && /tmp/amgr_test
 *
 * Drives the js_task-only hooks directly: records persist across stop,
 * rename absorbs stale records, view follows the foreground switch and
 * the stopped-LRU eviction frees a row when the table is full.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app/mqjs_app_manager.h"
#include "app/mqjs_app_manager_internal.h"

static int count(void)
{
    mqjs_app_snapshot_t s[32];
    return mqjs_apps_snapshot(s, 32);
}

int main(void)
{
    mqjs_apps_init();
    assert(count() == 0);

    /* start creates a RUNNING record */
    mqjs_app_record_on_start("launcher", 0, MQJS_APP_KIND_SYSTEM, 1000);
    mqjs_app_record_on_start("circuit", 2, MQJS_APP_KIND_APP, 1001);
    const mqjs_app_snapshot_t *r = mqjs_app_record_find("circuit");
    assert(r && r->state == MQJS_APP_RUNNING && r->kind == MQJS_APP_KIND_APP);
    assert(mqjs_app_record_find("launcher")->kind == MQJS_APP_KIND_SYSTEM);
    printf("ok start -> RUNNING records\n");

    /* foreground switch flips views */
    mqjs_app_record_set_view("launcher", MQJS_APP_VIEW_BACKGROUND);
    mqjs_app_record_set_view("circuit", MQJS_APP_VIEW_FOREGROUND);
    mqjs_app_record_touch("circuit", 2000);
    assert(mqjs_app_record_find("circuit")->view == MQJS_APP_VIEW_FOREGROUND);
    assert(mqjs_app_record_find("circuit")->last_active_ms == 2000);
    printf("ok view + last_active follow focus\n");

    /* stop: the record SURVIVES (the point of the App/Worker split) */
    mqjs_app_record_on_stop("circuit", MQJS_APP_STOP_USER);
    r = mqjs_app_record_find("circuit");
    assert(r && r->state == MQJS_APP_STOPPED && r->view == MQJS_APP_VIEW_NONE);
    assert(count() == 2);
    printf("ok stop -> record persists as STOPPED\n");

    /* restart reuses the record (same id) */
    mqjs_app_id_t old_id = r->id;
    mqjs_app_record_on_start("circuit", 3, MQJS_APP_KIND_APP, 3000);
    r = mqjs_app_record_find("circuit");
    assert(r->state == MQJS_APP_RUNNING && r->id == old_id);
    printf("ok restart reuses the record\n");

    /* setAppName rename absorbs a stale record under the new name */
    mqjs_app_record_on_start("dev_idle", 1, MQJS_APP_KIND_APP, 4000);
    mqjs_app_record_on_stop("dev_idle", MQJS_APP_STOP_USER);
    mqjs_app_record_on_start("probe_x", 1, MQJS_APP_KIND_APP, 5000);
    mqjs_app_record_on_rename("probe_x", "dev_idle");
    assert(mqjs_app_record_find("probe_x") == NULL);
    r = mqjs_app_record_find("dev_idle");
    assert(r && r->state == MQJS_APP_RUNNING);
    printf("ok rename absorbs the stale record\n");

    /* fill the table: the longest-stopped record is evicted, running
       records never are */
    for (int i = 0; i < 40; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "filler%d", i);
        mqjs_app_record_on_start(nm, 2, MQJS_APP_KIND_APP, 10000 + i);
        mqjs_app_record_on_stop(nm, MQJS_APP_STOP_USER);
    }
    assert(mqjs_app_record_find("launcher") != NULL); /* running: kept */
    assert(mqjs_app_record_find("dev_idle") != NULL);
    assert(mqjs_app_record_find("circuit") != NULL);
    assert(mqjs_app_record_find("filler0") == NULL);  /* oldest: evicted */
    assert(mqjs_app_record_find("filler39") != NULL); /* newest: kept */
    printf("ok stopped-LRU eviction under table pressure\n");

    printf("app_manager selftest: ALL PASS\n");
    return 0;
}
