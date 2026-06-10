/* PC runner for example scripts (no hardware needed).
   gpio.* are stubs that print; timers run for real.

   Scripts that keep timers/handlers alive run forever, same as on the
   device - wrap with `timeout` when smoke-testing:

     timeout 3 ./run_pc ../../examples/life.js
*/
#include <stdio.h>
#include <stdlib.h>
#include "mqjs_runtime.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s script.js\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc(len + 1);
    if (fread(src, 1, len, f) != (size_t)len) {
        perror("fread");
        return 2;
    }
    src[len] = 0;
    fclose(f);

    size_t mem_size = 256 * 1024;   /* same as JS_MEM_SIZE on the device */
    void *mem = malloc(mem_size);
    int rc = mqjs_run_script(src, len, argv[1], mem, mem_size);
    printf("rc = %d\n", rc);
    return rc < 0 ? 1 : 0;
}
