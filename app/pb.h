#pragma once
/*
 * Codec protobuf minimal (wire format) — juste ce qu'il faut pour parler à
 * meshtasticd via l'API Stream TCP (pas de nanopb, empreinte minuscule).
 * Wire types : 0=varint, 1=fixed64, 2=length-delimited, 5=fixed32.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ----------------------------------------------------------------- lecture */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} pb_reader;

static inline void pb_reader_init(pb_reader *r, const uint8_t *buf, size_t len) {
    r->p = buf;
    r->end = buf + len;
}
static inline bool pb_eof(const pb_reader *r) { return r->p >= r->end; }

bool pb_read_varint(pb_reader *r, uint64_t *out);
bool pb_read_tag(pb_reader *r, uint32_t *field, uint32_t *wire);
bool pb_read_fixed32(pb_reader *r, uint32_t *out);
bool pb_read_fixed64(pb_reader *r, uint64_t *out);
bool pb_read_bytes(pb_reader *r, const uint8_t **data, size_t *len);
bool pb_skip(pb_reader *r, uint32_t wire);

float pb_as_float(uint32_t bits);

/* ----------------------------------------------------------------- écriture */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    bool     ovf;   /* débordement détecté */
} pb_writer;

void pb_writer_init(pb_writer *w, uint8_t *buf, size_t cap);
void pb_put_varint(pb_writer *w, uint64_t v);
void pb_put_raw(pb_writer *w, const uint8_t *data, size_t len);
void pb_field_varint(pb_writer *w, uint32_t field, uint64_t v);
void pb_field_fixed32(pb_writer *w, uint32_t field, uint32_t v);
void pb_field_bool(pb_writer *w, uint32_t field, bool v);
void pb_field_bytes(pb_writer *w, uint32_t field, const uint8_t *data, size_t len);
