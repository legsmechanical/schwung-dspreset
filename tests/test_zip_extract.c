#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dsp/dspreset/library_input.h"
#include "../src/dsp/dspreset/zip_extract.h"

typedef struct {
    const char *archive;
    const char *destination;
    int extracted;
} extract_context_t;

static int extract_preset(const ds_zip_entry_t *entry, void *opaque) {
    extract_context_t *context = opaque;
    char error[128] = {0};
    size_t len = strlen(entry->name);
    if (!ds_library_archive_entry_is_safe(entry->name) || len < 9 ||
        strcmp(entry->name + len - 9, ".dspreset") != 0) return 0;
    if (ds_zip_extract_entry(context->archive, context->destination, entry,
                             error, sizeof(error)) != 0) {
        fprintf(stderr, "extract failed: %s\n", error);
        return 1;
    }
    context->extracted++;
    return 0;
}

int main(int argc, char **argv) {
    char error[128] = {0};
    char result[1024];
    FILE *f;
    extract_context_t context;
    assert(argc == 3);
    context = (extract_context_t){argv[1], argv[2], 0};
    assert(ds_zip_visit_entries(argv[1], extract_preset, &context, error, sizeof(error)) == 0);
    assert(context.extracted == 1);
    assert(snprintf(result, sizeof(result), "%s/Capture GO-TO Bass/Capture GO-TO Bass.dspreset",
                    argv[2]) < (int)sizeof(result));
    f = fopen(result, "rb");
    assert(f != NULL);
    fclose(f);
    puts("zip extraction test passed");
    return 0;
}
