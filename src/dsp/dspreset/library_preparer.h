#ifndef DSPRESET_LIBRARY_PREPARER_H
#define DSPRESET_LIBRARY_PREPARER_H

#include <stdint.h>

typedef struct {
    uint32_t extracted_files;
    uint32_t preset_files;
    uint32_t sample_files;
} ds_library_prepare_result_t;

/* Imports an archive to `destination` transactionally.  It extracts into a
 * sibling `.importing` directory, validates that a DSPreset and sample data
 * exist, then renames the completed tree into place.  Intended solely for a
 * background worker; the render thread must receive only the READY directory. */
int ds_library_prepare_archive(const char *archive_path, const char *destination,
                               ds_library_prepare_result_t *result,
                               char *error, unsigned error_len);

#endif
