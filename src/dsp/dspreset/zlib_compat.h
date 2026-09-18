#ifndef DSPRESET_ZLIB_COMPAT_H
#define DSPRESET_ZLIB_COMPAT_H

/* Minimal stable zlib ABI needed by the non-real-time DSLibrary importer.
 * Keeping this declaration local lets the Move cross-build link its runtime
 * libz without requiring a host zlib development package. */
typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef void *voidpf;
typedef voidpf (*alloc_func)(voidpf, uInt, uInt);
typedef void (*free_func)(voidpf, voidpf);

typedef struct z_stream_s {
    const Bytef *next_in;
    uInt avail_in;
    uLong total_in;
    Bytef *next_out;
    uInt avail_out;
    uLong total_out;
    const char *msg;
    void *state;
    alloc_func zalloc;
    free_func zfree;
    voidpf opaque;
    int data_type;
    uLong adler;
    uLong reserved;
} z_stream;

#define Z_NO_FLUSH 0
#define Z_FINISH 4
#define Z_OK 0
#define Z_STREAM_END 1
#define MAX_WBITS 15

extern const char *zlibVersion(void);
extern int inflateInit2_(z_stream *stream, int window_bits, const char *version, int stream_size);
extern int inflate(z_stream *stream, int flush);
extern int inflateEnd(z_stream *stream);

#define inflateInit2(stream, window_bits) inflateInit2_((stream), (window_bits), zlibVersion(), (int)sizeof(z_stream))

#endif
