/* Embedding the Dawn VM from C: load an image, register a host module, run it.
 *
 *   cc examples/embed.c -Iinclude -Lbuild/lib -ldawn -o embed
 *   LD_LIBRARY_PATH=build/lib ./embed program.dream
 */
#include <stdio.h>
#include <string.h>

#include "dream/dream.h"

/* A host function Dawn code reaches as `host.shout!`. Arguments arrive already
 * forced, because the registration marks them strict. */
static dream_result host_shout(dream_process* p, const dream_value* args, uint32_t argc,
                              dream_value* out) {
    uint32_t len = 0;
    const char* s = dream_value_string(args[0], &len);
    if (!s) {
        *out = dream_make_error(p, "type_error", "shout! wants a string");
        return DREAM_BAD;
    }
    for (uint32_t i = 0; i < len; ++i) {
        int c = (unsigned char)s[i];
        putchar(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    putchar('\n');
    *out = dream_make_unit();
    return DREAM_OK;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: embed <image.dream>\n");
        return 2;
    }

    dream_vm* vm = dream_vm_new();

    const char* names[] = {"shout!"};
    const uint32_t arities[] = {1};
    const uint32_t strict[] = {1};
    const dream_native_fn fns[] = {host_shout};
    dream_vm_register_module(vm, "host", names, arities, strict, fns, 1);

    char err[256];
    if (!DREAM_IS_OK(dream_vm_load_file(vm, argv[1], err, sizeof err))) {
        fprintf(stderr, "load failed: %s\n", err);
        dream_vm_free(vm);
        return 1;
    }

    dream_result r = dream_vm_run(vm, NULL);
    printf("result: %s\n", dream_vm_result_text(vm));
    printf("reductions: %llu\n", (unsigned long long)dream_vm_reductions(vm));

    int failed = dream_vm_failed(vm);
    dream_vm_free(vm);
    return DREAM_IS_OK(r) && !failed ? 0 : 1;
}
