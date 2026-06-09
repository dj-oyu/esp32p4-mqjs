/* PC smoke test for the mqjs runtime (no hardware needed).
   gpio.* are stubs that print; timers run for real. The script must
   end with no pending timers/handlers, or the loop runs forever
   (which is the desired behavior on the device). */
#include <stdio.h>
#include <stdlib.h>
#include "mqjs_runtime.h"

static const char demo_js[] =
"print('hello from mquickjs');\n"
"gpio.setMode(2, gpio.OUT);\n"
"var n = 0;\n"
"var id = setInterval(function() {\n"
"    n++;\n"
"    gpio.write(2, n & 1);\n"
"    if (n === 4) {\n"
"        clearInterval(id);\n"
"        print('blink done, n =', n);\n"
"    }\n"
"}, 100);\n"
"setTimeout(function() { print('one-shot at n =', n); }, 250);\n"
"setTimeout(function() { print('bye at t>=600ms'); }, 600);\n";

int main(void)
{
    size_t mem_size = 64 * 1024;
    void *mem = malloc(mem_size);
    int rc = mqjs_run_script(demo_js, sizeof(demo_js) - 1, "demo", mem, mem_size);
    printf("rc = %d\n", rc);
    free(mem);
    return rc;
}
