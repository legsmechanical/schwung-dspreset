#include "library_preparer.h"

#include "library_input.h"
#include "zip_extract.h"
#include "zip_index.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PREPARE_PATH_BYTES 1024

typedef struct {
    const char *archive;
    const char *staging;
    ds_library_prepare_result_t result;
    char *error;
    unsigned error_len;
} prepare_context_t;

static int has_suffix_ci(const char *text, const char *suffix) {
    size_t text_len = strlen(text), suffix_len = strlen(suffix);
    if (text_len < suffix_len) return 0;
    text += text_len - suffix_len;
    while (*suffix) {
        char a = *text++, b = *suffix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 'a' - 'A');
        if (b >= 'A' && b <= 'Z') b = (char)(b + 'a' - 'A');
        if (a != b) return 0;
    }
    return 1;
}

static int import_entry(const ds_zip_entry_t *entry, void *opaque) {
    prepare_context_t *context = opaque;
    if (!ds_library_archive_entry_is_safe(entry->name)) return 0;
    if (ds_zip_extract_entry(context->archive, context->staging, entry,
                             context->error, context->error_len)) return 1;
    context->result.extracted_files++;
    if (has_suffix_ci(entry->name, ".dspreset")) context->result.preset_files++;
    if (has_suffix_ci(entry->name, ".wav") || has_suffix_ci(entry->name, ".flac")) {
        context->result.sample_files++;
    }
    return 0;
}

int ds_library_prepare_archive(const char *archive_path, const char *destination,
                               ds_library_prepare_result_t *result,
                               char *error, unsigned error_len) {
    char staging[PREPARE_PATH_BYTES];
    prepare_context_t context;
    struct stat st;
    if (!archive_path || !destination || !result) return -1;
    if (snprintf(staging, sizeof(staging), "%s.importing", destination) >= (int)sizeof(staging)) {
        if (error_len) snprintf(error, error_len, "%s", "import destination is too long");
        return -1;
    }
    if (stat(destination, &st) == 0 || stat(staging, &st) == 0) {
        if (error_len) snprintf(error, error_len, "%s", "import destination already exists");
        return -1;
    }
    if (mkdir(staging, 0755)) {
        if (error_len) snprintf(error, error_len, "%s", "cannot create import staging directory");
        return -1;
    }
    context = (prepare_context_t){archive_path, staging, {0}, error, error_len};
    if (ds_zip_visit_entries(archive_path, import_entry, &context, error, error_len) ||
        !context.result.preset_files || !context.result.sample_files) {
        if (error_len && !*error) snprintf(error, error_len, "%s", "archive has no playable DSPreset samples");
        return -1;
    }
    if (rename(staging, destination)) {
        if (error_len) snprintf(error, error_len, "%s", "cannot finalize library import");
        return -1;
    }
    *result = context.result;
    return 0;
}
