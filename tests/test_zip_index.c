#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/dsp/dspreset/zip_index.h"
#include "../src/dsp/dspreset/library_input.h"

static int count_preset(const ds_zip_entry_t *entry, void *context) {
    int *found = context;
    size_t len = strlen(entry->name);
    if (ds_library_archive_entry_is_safe(entry->name) && len >= 9 &&
        strcmp(entry->name + len - 9, ".dspreset") == 0) {
        (*found)++;
    }
    return 0;
}

int main(int argc, char **argv) {
    char error[128] = {0};
    int presets = 0;
    assert(argc == 2);
    assert(ds_zip_visit_entries(argv[1], count_preset, &presets, error, sizeof(error)) == 0);
    assert(presets == 1);
    puts("zip index test passed");
    return 0;
}
