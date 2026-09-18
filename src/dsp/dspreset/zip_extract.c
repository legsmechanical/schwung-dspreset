#include "zip_extract.h"

#include "library_input.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#define COPY_BUFFER_BYTES 65536
#define PATH_BUFFER_BYTES 1024

static unsigned le16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16);
}

static void fail(char *out, unsigned n, const char *message) {
    if (n) snprintf(out, n, "%s", message);
}

static int make_parent_directories(char *path, char *error, unsigned error_len) {
    char *p;
    for (p = path + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0755) && errno != EEXIST) {
            fail(error, error_len, "cannot create import directory");
            *p = '/';
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static int write_all(int fd, const unsigned char *data, size_t len) {
    while (len) {
        ssize_t written = write(fd, data, len);
        if (written <= 0) return -1;
        data += written;
        len -= (size_t)written;
    }
    return 0;
}

int ds_zip_extract_entry(const char *archive_path, const char *destination_root,
                         const ds_zip_entry_t *entry, char *error, unsigned error_len) {
    unsigned char header[30], buffer[COPY_BUFFER_BYTES], output_buffer[COPY_BUFFER_BYTES];
    char output_path[PATH_BUFFER_BYTES];
    int input = -1, output = -1;
    uint64_t remaining;
    ssize_t got;
    z_stream stream = {0};

    if (!archive_path || !destination_root || !entry || !entry->name ||
        !ds_library_archive_entry_is_safe(entry->name)) {
        fail(error, error_len, "unsafe ZIP entry");
        return -1;
    }
    if (entry->compression_method != 0 && entry->compression_method != 8) {
        fail(error, error_len, "ZIP compression is not supported by this importer");
        return -1;
    }
    if ((entry->compression_method == 0 && entry->compressed_size != entry->uncompressed_size) ||
        entry->local_header_offset > INT64_MAX) {
        fail(error, error_len, "invalid ZIP entry");
        return -1;
    }
    if (snprintf(output_path, sizeof(output_path), "%s/%s", destination_root,
                 entry->name) >= (int)sizeof(output_path)) {
        fail(error, error_len, "import path is too long");
        return -1;
    }
    if (make_parent_directories(output_path, error, error_len)) return -1;

    input = open(archive_path, O_RDONLY);
    if (input < 0 || lseek(input, (off_t)entry->local_header_offset, SEEK_SET) < 0 ||
        read(input, header, sizeof(header)) != (ssize_t)sizeof(header) ||
        le32(header) != 0x04034b50) {
        fail(error, error_len, "cannot read ZIP local entry");
        goto failed;
    }
    if (lseek(input, (off_t)le16(header + 26) + (off_t)le16(header + 28), SEEK_CUR) < 0) {
        fail(error, error_len, "invalid ZIP local entry");
        goto failed;
    }
    output = open(output_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (output < 0) {
        fail(error, error_len, "cannot create imported file");
        goto failed;
    }
    remaining = entry->compressed_size;
    if (entry->compression_method == 8 && inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        fail(error, error_len, "cannot initialize ZIP inflater");
        goto failed;
    }
    while (remaining) {
        size_t want = remaining > sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        got = read(input, buffer, want);
        if (got <= 0) { fail(error, error_len, "truncated ZIP entry data"); goto failed; }
        if (entry->compression_method == 0) {
            if (write_all(output, buffer, (size_t)got)) {
                fail(error, error_len, "cannot write imported file"); goto failed;
            }
        } else {
            stream.next_in = buffer;
            stream.avail_in = (uInt)got;
            do {
                int status;
                stream.next_out = output_buffer;
                stream.avail_out = sizeof(output_buffer);
                status = inflate(&stream, Z_NO_FLUSH);
                if (status != Z_OK && status != Z_STREAM_END) {
                    fail(error, error_len, "cannot inflate ZIP entry"); goto failed;
                }
                if (write_all(output, output_buffer, sizeof(output_buffer) - stream.avail_out)) {
                    fail(error, error_len, "cannot write imported file"); goto failed;
                }
                if (status == Z_STREAM_END && stream.avail_in) {
                    fail(error, error_len, "invalid ZIP entry data"); goto failed;
                }
            } while (stream.avail_in);
        }
        remaining -= (uint64_t)got;
    }
    if (entry->compression_method == 8) {
        int status = inflate(&stream, Z_FINISH);
        if (status != Z_STREAM_END || stream.total_out != entry->uncompressed_size) {
            fail(error, error_len, "truncated inflated ZIP entry"); goto failed;
        }
        inflateEnd(&stream);
    }
    close(output);
    close(input);
    return 0;

failed:
    if (entry->compression_method == 8) inflateEnd(&stream);
    if (output >= 0) close(output);
    if (input >= 0) close(input);
    return -1;
}
