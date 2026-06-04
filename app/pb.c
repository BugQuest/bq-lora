#include "pb.h"
#include <string.h>

/* ----------------------------------------------------------------- lecture */
bool pb_read_varint(pb_reader *r, uint64_t *out)
{
    uint64_t v = 0;
    int shift = 0;
    while (r->p < r->end && shift < 64) {
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = v; return true; }
        shift += 7;
    }
    return false;
}

bool pb_read_tag(pb_reader *r, uint32_t *field, uint32_t *wire)
{
    uint64_t t;
    if (!pb_read_varint(r, &t)) return false;
    *field = (uint32_t)(t >> 3);
    *wire  = (uint32_t)(t & 0x7);
    return true;
}

bool pb_read_fixed32(pb_reader *r, uint32_t *out)
{
    if (r->end - r->p < 4) return false;
    *out = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
           ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return true;
}

bool pb_read_fixed64(pb_reader *r, uint64_t *out)
{
    if (r->end - r->p < 8) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r->p[i] << (8 * i);
    r->p += 8;
    *out = v;
    return true;
}

bool pb_read_bytes(pb_reader *r, const uint8_t **data, size_t *len)
{
    uint64_t n;
    if (!pb_read_varint(r, &n)) return false;
    if ((uint64_t)(r->end - r->p) < n) return false;
    *data = r->p;
    *len  = (size_t)n;
    r->p += n;
    return true;
}

bool pb_skip(pb_reader *r, uint32_t wire)
{
    uint64_t tmp;
    const uint8_t *d;
    size_t l;
    switch (wire) {
        case 0: return pb_read_varint(r, &tmp);
        case 1: return pb_read_fixed64(r, &tmp);
        case 2: return pb_read_bytes(r, &d, &l);
        case 5: { uint32_t t; return pb_read_fixed32(r, &t); }
        default: return false;
    }
}

float pb_as_float(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* ----------------------------------------------------------------- écriture */
void pb_writer_init(pb_writer *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ovf = false;
}

static void put_byte(pb_writer *w, uint8_t b)
{
    if (w->len < w->cap) w->buf[w->len++] = b;
    else w->ovf = true;
}

void pb_put_raw(pb_writer *w, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) put_byte(w, data[i]);
}

void pb_put_varint(pb_writer *w, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        put_byte(w, b);
    } while (v);
}

static void put_tag(pb_writer *w, uint32_t field, uint32_t wire)
{
    pb_put_varint(w, ((uint64_t)field << 3) | wire);
}

void pb_field_varint(pb_writer *w, uint32_t field, uint64_t v)
{
    put_tag(w, field, 0);
    pb_put_varint(w, v);
}

void pb_field_fixed32(pb_writer *w, uint32_t field, uint32_t v)
{
    put_tag(w, field, 5);
    put_byte(w, v & 0xff);
    put_byte(w, (v >> 8) & 0xff);
    put_byte(w, (v >> 16) & 0xff);
    put_byte(w, (v >> 24) & 0xff);
}

void pb_field_bool(pb_writer *w, uint32_t field, bool v)
{
    pb_field_varint(w, field, v ? 1 : 0);
}

void pb_field_bytes(pb_writer *w, uint32_t field, const uint8_t *data, size_t len)
{
    put_tag(w, field, 2);
    pb_put_varint(w, len);
    pb_put_raw(w, data, len);
}
