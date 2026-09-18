#include "zip_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16); }
static void fail(char *out, unsigned n, const char *msg) { if (n) { snprintf(out, n, "%s", msg); } }

int ds_zip_visit_entries(const char *path, ds_zip_entry_visitor_t visit, void *ctx,
                         char *error, unsigned error_len) {
    FILE *f = fopen(path, "rb");
    long size, start, eocd = -1, central_off, central_size;
    unsigned char *tail = NULL, *central = NULL;
    unsigned entries, i = 0; int rc = -1;
    if (!f) { fail(error, error_len, "cannot open archive"); return -1; }
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 22) { fail(error, error_len, "invalid ZIP size"); goto done; }
    start = size > 65557 ? size - 65557 : 0;
    tail = malloc((size_t)(size - start));
    if (!tail || fseek(f, start, SEEK_SET) || fread(tail, 1, (size_t)(size - start), f) != (size_t)(size - start)) { fail(error, error_len, "cannot read ZIP footer"); goto done; }
    for (long p = size - start - 22; p >= 0; --p) if (le32(tail + p) == 0x06054b50) { eocd = p; break; }
    if (eocd < 0 || le16(tail + eocd + 4) || le16(tail + eocd + 6)) { fail(error, error_len, "unsupported ZIP directory"); goto done; }
    entries = le16(tail + eocd + 10); central_size = (long)le32(tail + eocd + 12); central_off = (long)le32(tail + eocd + 16);
    if (central_size < 0 || central_size > 16 * 1024 * 1024 || central_off < 0 || central_off + central_size > size) { fail(error, error_len, "invalid ZIP central directory"); goto done; }
    central = malloc((size_t)central_size); if (!central || fseek(f, central_off, SEEK_SET) || fread(central, 1, (size_t)central_size, f) != (size_t)central_size) { fail(error, error_len, "cannot read ZIP directory"); goto done; }
    for (unsigned char *p = central; i < entries; ++i) {
        unsigned name_len, extra_len, comment_len; ds_zip_entry_t e; char *name;
        if (p + 46 > central + central_size || le32(p) != 0x02014b50) { fail(error, error_len, "malformed ZIP entry"); goto done; }
        name_len=le16(p+28); extra_len=le16(p+30); comment_len=le16(p+32);
        if (p + 46 + name_len + extra_len + comment_len > central + central_size) { fail(error,error_len,"truncated ZIP entry"); goto done; }
        name=malloc(name_len+1); if (!name) { fail(error,error_len,"out of memory"); goto done; }
        memcpy(name,p+46,name_len); name[name_len]=0;
        e=(ds_zip_entry_t){name,le16(p+10),le32(p+20),le32(p+24),le32(p+42)};
        if (visit(&e,ctx)) { free(name); rc=0; goto done; } free(name);
        p += 46 + name_len + extra_len + comment_len;
    }
    rc=0;
done: free(central); free(tail); fclose(f); return rc;
}
