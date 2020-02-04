#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define panic(fmt, ...)                                   \
    do {                                                  \
        fprintf(stderr, "ERR: " fmt "\n", ##__VA_ARGS__); \
        exit(1);                                          \
    } while (0)

#define xassert(c, fmt, ...)                                     \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "ASSERT: " fmt "\n", ##__VA_ARGS__); \
            assert(0);                                           \
        }                                                        \
    } while (0)

#define ARRSIZE(x) (sizeof(x) / sizeof(x[0]))

struct gzip_header
{
    uint8_t id1;
    uint8_t id2;
    uint8_t cm;
    uint8_t flg;
    uint32_t mtime;
    uint8_t xfl;
    uint8_t os;
} __attribute__((packed));
typedef struct gzip_header gzip_header;

static const uint8_t ID1_GZIP = 31;
static const uint8_t ID2_GZIP = 139;

// Header Flags
static const uint8_t FTEXT    = 1u << 0;
static const uint8_t FHCRC    = 1u << 1;
static const uint8_t FEXTRA   = 1u << 2;
static const uint8_t FNAME    = 1u << 3;
static const uint8_t FCOMMENT = 1u << 4;
static const uint8_t RESERV1  = 1u << 5;
static const uint8_t RESERV2  = 1u << 6;
static const uint8_t RESERV3  = 1u << 7;

// Block Types
static const uint8_t NO_COMPRESSION   = 0x0u;
static const uint8_t FIXED_HUFFMAN    = 0x1u;
static const uint8_t DYNAMIC_HUFFMAN  = 0x2u;
static const uint8_t RESERVED         = 0x3u;

struct stream
{
    const uint8_t *beg;
    const uint8_t *cur; /* beg <= cur <= end */
    const uint8_t *end;
    int            error; /* 0 = OK, otherwise returns result of ferror */
    int          (*refill)(struct stream* s);
    void          *udata;
};
typedef struct stream stream;

static const uint8_t _zeros[256] = { 0 };

int refill_zeros(stream *s)
{
    s->beg = &_zeros[0];
    s->cur = s->beg;
    s->end = s->beg + sizeof(_zeros);
    return s->error;
}

void init_zeros_stream(stream *s)
{
    s->refill = &refill_zeros;
    s->error = 0;
    s->udata = NULL;
    s->refill(s);
}

struct file_stream
{
    uint8_t buf[2048];
    // uint8_t buf[1];
    FILE   *fp;
};
typedef struct file_stream file_stream;

int refill_file(stream *s)
{
    struct file_stream *d = s->udata;
    size_t rem = s->end - s->cur;
    size_t read;
    assert(s->beg <= s->cur && s->cur <= s->end);
    memmove(&d->buf[0], s->cur, rem);
    read = fread(&d->buf[rem], 1, sizeof(d->buf) - rem, d->fp);
    if (read > 0) {
        s->beg = &d->buf[0];
        s->cur = &d->buf[rem];
        s->end = &d->buf[rem+read];
        // TODO(peter): check ferror if `read != sizeof(d->buf) - rem`?
        assert(s->beg <= s->cur && s->cur <= s->end);
        assert(s->end <= &d->buf[sizeof(d->buf)]);
        return 0;
    } else {
        init_zeros_stream(s);
        s->error = ferror(d->fp);
        return s->error;
    }
}

void init_file_stream(stream *s, file_stream* data)
{
    s->beg = &data->buf[0];
    s->cur = &data->buf[0];
    s->end = &data->buf[0];
    s->error = 0;
    s->refill = &refill_file;
    s->udata = data;
    s->refill(s);
}

#define MIN(x, y) (x) < (y) ? (x) : (y)

int stream_read(stream* s, void* buf, size_t n)
{
    /* fast path: plenty of data available */
    if (s->end - s->cur >= n) {
        memcpy(buf, s->cur, n);
        s->cur += n;
        return 0;
    }

    uint8_t *p = buf;
    while (n > 0) {
        if (s->cur == s->end) {
            if (s->refill(s) != 0)
                return s->error;
        }
        size_t avail = MIN(n, s->end - s->cur);
        memcpy(p, s->cur, avail);
        s->cur += avail;
        p      += avail;
        n      -= avail;
    }
    return 0;
}

char *read_null_terminated_string(stream *s)
{
    size_t pos;
    size_t len = 0;
    char *str = NULL;
    for (;;) {
        size_t n = s->end - s->cur;
        const uint8_t *p = memchr(s->cur, '\0', n);
        if (p) {
            pos = p - s->cur + 1;
            str = realloc(str, len + pos);
            if (!str) return NULL;
            memcpy(&str[len], s->cur, pos);
            assert(str[len + pos - 1] == '\0');
            s->cur += pos;
            return str;
        } else {
            str = realloc(str, len + n);
            if (!str) return NULL;
            memcpy(&str[len], s->cur, n);
            assert(s->cur + n == s->end);
            s->cur = s->end;
            if (s->refill(s) != 0) {
                free(str);
                return NULL;
            }
            len += n;
        }
    }
}

uint8_t readbits(size_t nbits, stream* s, size_t *bitpos)
{
    uint8_t result = 0;
    while (nbits-- > 0) {
        if (*bitpos == 8) {
            ++s->cur;
            if (s->cur == s->end)
                if (s->refill(s) != 0)
                    panic("stream read error: %d", s->error);
            *bitpos = 0;
        }
        result <<= 1;
        result |= ((s->cur[0] >> *bitpos) & 0x1u);
        ++*bitpos;
    }
    return result;
}

int main(int argc, char** argv) {
    FILE* fp;
    FILE* out;
    char* input_filename = argv[1];
    char* output_filename;
    gzip_header hdr;
    file_stream file_stream_data;
    stream strm;

    if (argc == 2) {
        output_filename = NULL;
    } else if (argc == 3) {
        output_filename = argv[2];
    } else {
        fprintf(stderr, "Usage: %s [FILE] [OUT]\n", argv[0]);
        exit(0);
    }

    file_stream_data.fp = fopen(input_filename, "rb");
    if (!file_stream_data.fp) {
        panic("failed to open input file: %s", input_filename);
    }

    out = output_filename ? fopen(output_filename, "wb") : stdout;
    if (!out) {
        panic("failed to open output file: %s", output_filename ?: "<stdout>");
    }

    init_file_stream(&strm, &file_stream_data);

    /**************************************************************************
     * Read header and metadata
     *************************************************************************/

    if (stream_read(&strm, &hdr, sizeof(hdr)) != 0)
        panic("unable to read gzip header: %d", strm.error);

    printf("GzipHeader:\n");
    printf("\tid1   = %u (0x%02x)\n", hdr.id1, hdr.id1);
    printf("\tid2   = %u (0x%02x)\n", hdr.id2, hdr.id2);
    printf("\tcm    = %u\n", hdr.cm);
    printf("\tflg   = %u\n", hdr.flg);
    printf("\tmtime = %u\n", hdr.mtime);
    printf("\txfl   = %u\n", hdr.xfl);
    printf("\tos    = %u\n", hdr.os);

    if (hdr.id1 != ID1_GZIP)
        panic("Unsupported identifier #1: %u.", hdr.id1);
    if (hdr.id2 != ID2_GZIP)
        panic("Unsupported identifier #2: %u.", hdr.id2);
    if ((hdr.flg & FEXTRA) != 0) {
        // +---+---+=================================+
        // | XLEN  |...XLEN bytes of "extra field"...| (more-->)
        // +---+---+=================================+
        panic("FEXTRA flag not supported.");
    }
    if ((hdr.flg & FNAME) != 0) {
        // +=========================================+
        // |...original file name, zero-terminated...| (more-->)
        // +=========================================+
        char *orig_filename = read_null_terminated_string(&strm);
        if (!orig_filename)
            panic("unable to read original filename: %d", strm.error);
        printf("File contains original filename!: '%s'\n", orig_filename);
        free(orig_filename);
    }
    if ((hdr.flg & FCOMMENT) != 0) {
        // +===================================+
        // |...file comment, zero-terminated...| (more-->)
        // +===================================+
        printf("File contains comment\n");
        char *comment = read_null_terminated_string(&strm);
        if (!comment)
            panic("unable to read file comment: %d", strm.error);
        printf("File comment: '%s'\n", comment);
        free(comment);
    }
    if ((hdr.flg & FHCRC) != 0) {
        // +---+---+
        // | CRC16 |
        // +---+---+
        //
        // +=======================+
        // |...compressed blocks...| (more-->)
        // +=======================+
        //
        // 0   1   2   3   4   5   6   7
        // +---+---+---+---+---+---+---+---+
        // |     CRC32     |     ISIZE     |
        // +---+---+---+---+---+---+---+---+
        uint16_t crc16 = 0;
        if (stream_read(&strm, &crc16, sizeof(crc16)) != 0)
            panic("unable to read crc16: %d", strm.error);
        printf("CRC16: %u (0x%04X)\n", crc16, crc16);
    }
    if ((hdr.flg & (RESERV1 | RESERV2 | RESERV3)) != 0)
        panic("reserve bits are not 0");

    /**************************************************************************
     * Read compressed data
     *************************************************************************/

    /* know at this point that are on a byte boundary as all previous fields
     * have been byte sized */
    size_t bitpos = 0; 
    size_t nbits;
    uint8_t bfinal;
    do {
        bfinal = readbits(1, &strm, &bitpos);
        uint8_t blktype = readbits(2, &strm, &bitpos);
        printf("BFINAL? %u, bitpos = %zu\n", bfinal, bitpos);
        if (blktype == NO_COMPRESSION) {
            printf("No Compression Block\n");
        } else if (blktype == FIXED_HUFFMAN) {
            printf("Fixed Huffman Block\n");
        } else if (blktype == DYNAMIC_HUFFMAN) {
            printf("Dynamic Huffman Block\n");
        } else {
            panic("Invalid block type: %u", blktype);
        }
        break;
    } while (bfinal == 0);

    fclose(file_stream_data.fp);
    fclose(out);
    return 0;
}
