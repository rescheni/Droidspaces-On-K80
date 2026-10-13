// SPDX-License-Identifier: GPL-2.0-only
/* Validate a module's host layout/prototype constraints against running BTF. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

struct type { const unsigned char *p; unsigned kind, count, flag; };
static unsigned char *blob;
static const char *strings;
static size_t string_size;
static struct type *types;
static unsigned count;
static void fail(const char *why) { fprintf(stderr, "ABI: %s\n", why); exit(1); }
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static const char *name_at(unsigned offset)
{
    if (offset >= string_size || !memchr(strings + offset, 0, string_size - offset))
        fail("invalid BTF string");
    return strings + offset;
}
static struct type *type_at(unsigned id)
{
    if (!id || id >= count) fail("invalid BTF type reference");
    return &types[id];
}
static uint64_t type_size(unsigned id, unsigned depth)
{
    if (!id) return 0;
    if (depth > 128) fail("recursive BTF type");
    struct type *t = type_at(id);
    unsigned n = u32(t->p + 8);
    switch (t->kind) {
    case 8: case 9: case 10: case 11: case 18: return type_size(n, depth + 1);
    case 2: return 8;
    case 3: return type_size(u32(t->p + 12), depth + 1) * u32(t->p + 20);
    case 1: case 4: case 5: case 6: case 16: case 19: return n;
    default: return 0;
    }
}
struct field { const char *name; uint64_t offset, bits; };
static struct field fields[8192];
static unsigned nfields;
static void flatten(unsigned id, uint64_t base, unsigned depth)
{
    if (depth > 64) fail("recursive anonymous aggregate");
    struct type *t = type_at(id);
    for (unsigned i = 0; i < t->count; i++) {
        const unsigned char *m = t->p + 12 + i * 12;
        const char *name = name_at(u32(m));
        unsigned typ = u32(m + 4), offset = u32(m + 8);
        unsigned bits = t->flag ? offset >> 24 : 0;
        if (t->flag) offset &= 0xffffff;
        struct type *inner = type_at(typ);
        if (!*name && (inner->kind == 4 || inner->kind == 5)) {
            flatten(typ, base + offset, depth + 1);
        } else {
            unsigned j;
            /* Match the canonical flattened mapping for anonymous members. */
            for (j = 0; j < nfields; j++) if (!strcmp(fields[j].name, name)) break;
            if (j == nfields && nfields++ >= 8192) fail("too many aggregate fields");
            fields[j] = (struct field){name, base + offset, bits ? bits : type_size(typ, 0) * 8};
        }
    }
}
static uint64_t hash;
static void hash_bytes(const unsigned char *p, size_t n)
{
    while (n--) hash = (hash ^ *p++) * UINT64_C(1099511628211);
}
static void hash_word(uint32_t n)
{
    unsigned char bytes[4] = {n, n >> 8, n >> 16, n >> 24};
    hash_bytes(bytes, 4);
}
static void hash_type(unsigned id, unsigned depth)
{
    if (depth > 128) fail("recursive function prototype");
    if (!id) { hash_word(0); return; }
    struct type *t = type_at(id);
    unsigned n = u32(t->p + 8);
    const char *name = name_at(u32(t->p));
    switch (t->kind) {
    case 8: case 9: case 10: case 11: case 18: hash_type(n, depth + 1); break;
    case 2: hash_word(2); hash_type(n, depth + 1); break;
    case 13:
        hash_word(13); hash_type(n, depth + 1); hash_word(t->count);
        for (unsigned i = 0; i < t->count; i++) hash_type(u32(t->p + 16 + i * 8), depth + 1);
        break;
    case 1: hash_word(1); hash_word(n); hash_word(u32(t->p + 12)); break;
    case 4: case 5: case 7: hash_word(4); hash_bytes((const unsigned char *)name, strlen(name) + 1); break;
    case 3: hash_word(3); hash_type(u32(t->p + 12), depth + 1); hash_word(u32(t->p + 20)); break;
    default: hash_word(t->kind); hash_bytes((const unsigned char *)name, strlen(name) + 1); hash_word(n); break;
    }
}
static void load_btf(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot open kernel BTF");
    /* sysfs reports zero size, so read until EOF rather than using stat(). */
    const size_t limit = 64 * 1024 * 1024;
    blob = malloc(limit);
    if (!blob) fail("out of memory");
    size_t size = fread(blob, 1, limit, f);
    if (ferror(f) || !feof(f) || size < 24) fail("invalid BTF file size/read");
    fclose(f);
    if (blob[0] != 0x9f || blob[1] != 0xeb || blob[2] != 1) fail("expected little-endian BTF v1");
    uint64_t header = u32(blob + 4), start = header + u32(blob + 8);
    uint64_t end = start + u32(blob + 12), str = header + u32(blob + 16);
    string_size = u32(blob + 20);
    if (header < 24 || start < header || end > size || str > size || string_size > size - str)
        fail("BTF section outside file");
    strings = (const char *)blob + str;
    types = calloc((end - start) / 12 + 1, sizeof(*types));
    if (!types) fail("out of memory");
    count = 1;
    while (start < end) {
        if (end - start < 12) fail("truncated BTF type");
        const unsigned char *p = blob + start;
        unsigned info = u32(p + 4), kind = (info >> 24) & 31, n = info & 65535, words;
        switch (kind) {
        case 1: case 14: case 17: words = 1; break;
        case 2: case 7: case 8: case 9: case 10: case 11: case 12: case 16: case 18: words = 0; break;
        case 3: words = 3; break;
        case 4: case 5: case 15: case 19: words = n * 3; break;
        case 6: case 13: words = n * 2; break;
        default: fail("unsupported BTF type kind"); return;
        }
        if (12 + words * 4 > end - start) fail("truncated BTF payload");
        name_at(u32(p));
        types[count++] = (struct type){p, kind, n, info >> 31};
        start += 12 + words * 4;
    }
}
static unsigned lookup(const char *name, int function)
{
    unsigned found = 0;
    for (unsigned i = 1; i < count; i++) {
        struct type *t = &types[i];
        if ((function ? t->kind == 12 : ((t->kind == 4 || t->kind == 5) && u32(t->p + 8))) &&
            !strcmp(name_at(u32(t->p)), name)) found = i;
    }
    return found;
}
int main(int argc, char **argv)
{
    int offline = argc == 4 && !strcmp(argv[1], "--offline");
    if (offline) { argc--; argv++; }
    if (argc != 3) { fprintf(stderr, "usage: abi-check [--offline] constraints kernel.btf\n"); return 2; }
    if (!offline && sysconf(_SC_PAGESIZE) != 4096)
        fail("this module requires a 4 KiB kernel page size");
    load_btf(argv[2]);
    FILE *f = fopen(argv[1], "r");
    if (!f) fail("cannot open ABI constraints");
    char line[1024], name[256], op;
    if (!fgets(line, sizeof(line), f) || strcmp(line, "DSABI1\n")) fail("invalid constraints version");
    unsigned layouts = 0, prototypes = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%c %255s", &op, name) != 2) fail("invalid constraint");
        unsigned id = lookup(name, op == 'P');
        if (!id) { fprintf(stderr, "ABI: missing %s\n", name); return 1; }
        if (op == 'S') {
            uint64_t size, expected;
            if (sscanf(line, "S %255s %" SCNu64 " %" SCNu64, name, &size, &expected) != 3) fail("invalid layout constraint");
            nfields = 0; flatten(id, 0, 0);
            if (u32(types[id].p + 8) != size || nfields != expected) {
                fprintf(stderr, "ABI: layout size/field count differs: %s\n", name); return 1;
            }
            for (unsigned i = 0; i < expected; i++) {
                char member[256]; uint64_t offset, bits;
                if (!fgets(line, sizeof(line), f) || sscanf(line, "F %255s %" SCNu64 " %" SCNu64, member, &offset, &bits) != 3)
                    fail("invalid field constraint");
                if (!strcmp(member, "-")) *member = 0;
                unsigned j;
                for (j = 0; j < nfields; j++) if (!strcmp(fields[j].name, member)) break;
                if (j == nfields || fields[j].offset != offset || fields[j].bits != bits) {
                    fprintf(stderr, "ABI: member differs: %s.%s\n", name, member); return 1;
                }
            }
            layouts++;
        } else if (op == 'P') {
            uint64_t expected;
            if (sscanf(line, "P %255s %" SCNx64, name, &expected) != 2) fail("invalid prototype constraint");
            hash = UINT64_C(14695981039346656037);
            hash_type(u32(types[id].p + 8), 0);
            if (hash != expected) { fprintf(stderr, "ABI: prototype differs: %s\n", name); return 1; }
            prototypes++;
        } else fail("unknown constraint type");
    }
    if (ferror(f) || !layouts || !prototypes) fail("empty or incomplete constraints");
    fclose(f);
    printf("ABI compatible: %u layouts, %u prototypes\n", layouts, prototypes);
    free(types); free(blob);
    return 0;
}
