#ifndef DSPRESET_ZIP_EXTRACT_H
#define DSPRESET_ZIP_EXTRACT_H

#include "zip_index.h"

/* Copies one stored (method 0) or deflated (method 8) entry below destination_root.  Both the ZIP
 * pathname and destination root are treated as untrusted boundaries: no
 * archive path may escape the private import directory.  Deflated entries
 * deliberately return an error until the importer is linked with its
 * non-real-time decompressor. */
int ds_zip_extract_entry(const char *archive_path, const char *destination_root,
                         const ds_zip_entry_t *entry, char *error, unsigned error_len);

#endif
