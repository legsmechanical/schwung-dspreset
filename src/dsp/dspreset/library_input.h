#ifndef DSPRESET_LIBRARY_INPUT_H
#define DSPRESET_LIBRARY_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS_LIBRARY_INPUT_UNSUPPORTED = 0,
    DS_LIBRARY_INPUT_PRESET_FILE,
    DS_LIBRARY_INPUT_DIRECTORY,
    DS_LIBRARY_INPUT_DSLIBRARY_ARCHIVE,
} ds_library_input_kind_t;

/* Classifies the user-selected library root. `.dslibrary` is a ZIP package
 * that owns a DSPreset and its assets; it is not a substitute preset format. */
ds_library_input_kind_t ds_classify_library_input(const char *path, int is_directory);

/* ZIP entry paths are untrusted input. This rejects archive traversal,
 * absolute paths, macOS resource-fork noise, and directory entries so a
 * package can only resolve assets beneath its private extraction/cache root. */
int ds_library_archive_entry_is_safe(const char *entry_path);

#ifdef __cplusplus
}
#endif

#endif
