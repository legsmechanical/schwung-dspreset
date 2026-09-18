#include "library_input.h"

#include <string.h>

static int has_suffix_ci(const char *text, const char *suffix) {
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    if (text_len < suffix_len) return 0;
    text += text_len - suffix_len;
    while (*suffix) {
        char a = *text++;
        char b = *suffix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return 0;
    }
    return 1;
}

ds_library_input_kind_t ds_classify_library_input(const char *path, int is_directory) {
    if (!path || !*path) return DS_LIBRARY_INPUT_UNSUPPORTED;
    if (is_directory) return DS_LIBRARY_INPUT_DIRECTORY;
    if (has_suffix_ci(path, ".dspreset")) return DS_LIBRARY_INPUT_PRESET_FILE;
    if (has_suffix_ci(path, ".dslibrary")) return DS_LIBRARY_INPUT_DSLIBRARY_ARCHIVE;
    return DS_LIBRARY_INPUT_UNSUPPORTED;
}

int ds_library_input_requires_prepare(ds_library_input_kind_t kind) {
    return kind == DS_LIBRARY_INPUT_DSLIBRARY_ARCHIVE;
}

int ds_library_archive_entry_is_safe(const char *entry_path) {
    const char *segment;
    const char *cursor;
    size_t segment_len;

    if (!entry_path || !*entry_path) return 0;
    if (entry_path[0] == '/' || entry_path[0] == '\\') return 0;
    if (strstr(entry_path, "__MACOSX/") == entry_path) return 0;
    if (strstr(entry_path, "/__MACOSX/") != NULL) return 0;

    segment = entry_path;
    for (cursor = entry_path;; cursor++) {
        if (*cursor != '/' && *cursor != '\\' && *cursor != '\0') continue;
        segment_len = (size_t)(cursor - segment);
        if (segment_len == 0 || (segment_len == 1 && segment[0] == '.')) return 0;
        if (segment_len == 2 && segment[0] == '.' && segment[1] == '.') return 0;
        if (*cursor == '\0') break;
        segment = cursor + 1;
    }
    return 1;
}
