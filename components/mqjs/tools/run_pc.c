/* PC runner for example scripts (no hardware needed).
   gpio.* are stubs that print; timers run for real.

   P4a: an optional second script runs as a concurrent app in slot 2,
   so the multi-app scheduler (timers across contexts, sys.signal /
   sys.onSignal, sys.focus + lifecycle callbacks) is testable on the
   host before touching hardware:

     ./run_pc ping.js pong.js

   Scripts that keep timers/handlers alive run forever, same as on the
   device - wrap with `timeout` when smoke-testing:

     timeout 3 ./run_pc ../../examples/life.js
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mqjs_runtime.h"

static char *load_file(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc(len + 1);
    if (fread(src, 1, len, f) != (size_t)len) {
        perror("fread");
        fclose(f);
        free(src);
        return NULL;
    }
    src[len] = 0;
    fclose(f);
    *out_len = len;
    return src;
}

/* app name = basename without extension (what sys.signal addresses) */
static const char *app_name(const char *path, char *buf, size_t cap)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    snprintf(buf, cap, "%s", base);
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf)
        *dot = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s script.js [slot2_script.js]\n", argv[0]);
        return 2;
    }
    long len = 0;
    char *src = load_file(argv[1], &len);
    if (!src)
        return 2;

    char name1[64], name2[64];
    if (argc >= 3) {
        long len2 = 0;
        char *src2 = load_file(argv[2], &len2);
        if (!src2)
            return 2;
        if (mqjs_app_start(2, src2, (size_t)len2,
                           app_name(argv[2], name2, sizeof name2))) {
            fprintf(stderr, "slot-2 app failed to start\n");
            return 1;
        }
    }

    size_t mem_size = 256 * 1024;   /* same as MQJS_APP_MEM_SIZE on device */
    void *mem = malloc(mem_size);
    int rc = mqjs_run_script(src, len, app_name(argv[1], name1, sizeof name1),
                             mem, mem_size);
    printf("rc = %d\n", rc);
    return rc < 0 ? 1 : 0;
}
