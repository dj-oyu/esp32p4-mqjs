/* Host tool: compile a JS task to relocatable mquickjs bytecode.
 *
 *   compile_task [-m64] in.js out.bin
 *
 * Default output is 32-bit bytecode for the ESP32-P4 (JSW=4); -m64
 * produces host-width bytecode so the result can be smoke-tested with
 * run_pc. Must be built against the SAME device stdlib as the firmware
 * (link with mqjs_runtime.c) so the parser sees identical atoms.
 *
 * The bytecode is not validated at load time on the device - delivery
 * relies on the Ed25519 signature check in task_source.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs.h"

extern const JSSTDLibraryDef js_stdlib;

static void js_log(void *opaque, const void *buf, size_t len)
{
    fwrite(buf, 1, len, stderr);
}

static uint8_t *load_file(const char *path, size_t *plen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) {
        perror("fread");
        exit(2);
    }
    buf[n] = '\0';
    fclose(f);
    *plen = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    int force_64 = 0, argi = 1;

    if (argi < argc && !strcmp(argv[argi], "-m64")) {
        force_64 = 1;
        argi++;
    }
    if (argc - argi != 2) {
        fprintf(stderr, "usage: %s [-m64] in.js out.bin\n", argv[0]);
        return 2;
    }
    const char *in_path = argv[argi], *out_path = argv[argi + 1];

    size_t mem_size = 8 * 1024 * 1024;
    void *mem = malloc(mem_size);
    JSContext *ctx = JS_NewContext2(mem, mem_size, &js_stdlib, 1);
    JS_SetLogFunc(ctx, js_log);

    size_t src_len;
    uint8_t *src = load_file(in_path, &src_len);
    JSValue val = JS_Parse(ctx, (char *)src, src_len, in_path, 0);
    if (JS_IsException(val)) {
        JSValue e = JS_GetException(ctx);
        fprintf(stderr, "%s: ", in_path);
        JS_PrintValueF(ctx, e, JS_DUMP_LONG);
        fprintf(stderr, "\n");
        return 1;
    }

    union {
        JSBytecodeHeader hdr;
        JSBytecodeHeader32 hdr32;
    } hdr_buf;
    size_t hdr_len;
    const uint8_t *data_buf;
    uint32_t data_len;

    if (force_64) {
        JS_PrepareBytecode(ctx, &hdr_buf.hdr, &data_buf, &data_len, val);
        /* relocate to base 0 for a deterministic file */
        JS_RelocateBytecode2(ctx, &hdr_buf.hdr, (uint8_t *)data_buf, data_len,
                             0, 0);
        hdr_len = sizeof(JSBytecodeHeader);
    } else {
        if (JS_PrepareBytecode64to32(ctx, &hdr_buf.hdr32, &data_buf,
                                     &data_len, val)) {
            fprintf(stderr, "64->32 bit bytecode conversion failed\n");
            return 1;
        }
        hdr_len = sizeof(JSBytecodeHeader32);
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        perror(out_path);
        return 2;
    }
    fwrite(&hdr_buf, 1, hdr_len, f);
    fwrite(data_buf, 1, data_len, f);
    fclose(f);

    printf("compiled %s -> %s (%zu bytes, %s)\n", in_path, out_path,
           hdr_len + data_len, force_64 ? "64-bit" : "32-bit");
    return 0;
}
