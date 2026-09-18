#ifndef DSPRESET_ZIP_INDEX_H
#define DSPRESET_ZIP_INDEX_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint16_t compression_method;
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint64_t local_header_offset;
} ds_zip_entry_t;

typedef int (*ds_zip_entry_visitor_t)(const ds_zip_entry_t *entry, void *context);

/* Reads only ZIP metadata. It never decompresses an entry and is therefore
 * suitable for the import worker's scan stage, never render_block. */
int ds_zip_visit_entries(const char *archive_path, ds_zip_entry_visitor_t visitor,
                         void *context, char *error, unsigned error_len);

#endif
