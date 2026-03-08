#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "WASMOSAP"
#define VERSION 1u
#define FLAG_DRIVER (1u << 0)
#define FLAG_SERVICE (1u << 1)
#define FLAG_APP (1u << 2)
#define FLAG_NEEDS_PRIV (1u << 3)

#define MEM_HINT_STACK 1u
#define MEM_HINT_HEAP 2u

typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t req_ep_count;
    uint32_t cap_count;
    uint32_t mem_hint_count;
    uint32_t reserved;
} wasmos_app_header_t;

typedef struct __attribute__((packed)) {
    uint32_t name_len;
    uint32_t rights;
} wasmos_req_endpoint_t;

typedef struct __attribute__((packed)) {
    uint32_t name_len;
    uint32_t flags;
} wasmos_cap_request_t;

typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t min_pages;
    uint32_t max_pages;
} wasmos_mem_hint_t;

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!s || !*s || (end && *end != '\0') || v > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 12) {
        fprintf(stderr, "usage: %s <in.wasm> <out.wasmosapp> <name> <entry> <stack_pages> <heap_pages> <flags> <req_ep_name|- > <req_ep_rights> <cap_name|- > <cap_flags>\n", argv[0]);
        return 1;
    }

    const char *in_path = argv[1];
    const char *out_path = argv[2];
    const char *name = argv[3];
    const char *entry = argv[4];
    const char *req_ep_name = argv[8];
    const char *cap_name = argv[10];
    uint32_t stack_pages = 0;
    uint32_t heap_pages = 0;
    uint32_t flags = 0;
    uint32_t req_ep_rights = 0;
    uint32_t cap_flags = 0;
    if (parse_u32(argv[5], &stack_pages) != 0 || parse_u32(argv[6], &heap_pages) != 0) {
        fprintf(stderr, "invalid stack/heap page value\n");
        return 1;
    }
    if (parse_u32(argv[7], &flags) != 0 ||
        parse_u32(argv[9], &req_ep_rights) != 0 || parse_u32(argv[11], &cap_flags) != 0) {
        fprintf(stderr, "invalid flags/req_ep_rights/cap_flags value\n");
        return 1;
    }
    int has_req_ep = !(req_ep_name[0] == '-' && req_ep_name[1] == '\0');
    int has_cap = !(cap_name[0] == '-' && cap_name[1] == '\0');

    FILE *in = fopen(in_path, "rb");
    if (!in) {
        perror("open input");
        return 1;
    }
    if (fseek(in, 0, SEEK_END) != 0) {
        perror("seek input");
        fclose(in);
        return 1;
    }
    long in_size = ftell(in);
    if (in_size < 0) {
        perror("size input");
        fclose(in);
        return 1;
    }
    if (fseek(in, 0, SEEK_SET) != 0) {
        perror("rewind input");
        fclose(in);
        return 1;
    }

    uint8_t *wasm = (uint8_t *)malloc((size_t)in_size);
    if (!wasm) {
        fclose(in);
        return 1;
    }
    if (fread(wasm, 1, (size_t)in_size, in) != (size_t)in_size) {
        perror("read input");
        fclose(in);
        free(wasm);
        return 1;
    }
    fclose(in);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("open output");
        free(wasm);
        return 1;
    }

    wasmos_app_header_t hdr;
    memcpy(hdr.magic, MAGIC, 8);
    hdr.version = VERSION;
    hdr.header_size = sizeof(hdr);
    hdr.flags = flags;
    hdr.name_len = (uint32_t)strlen(name);
    hdr.entry_len = (uint32_t)strlen(entry);
    hdr.wasm_size = (uint32_t)in_size;
    hdr.req_ep_count = has_req_ep ? 1u : 0u;
    hdr.cap_count = has_cap ? 1u : 0u;
    hdr.mem_hint_count = 2;
    hdr.reserved = 0;

    wasmos_mem_hint_t stack_hint = { MEM_HINT_STACK, stack_pages, 0 };
    wasmos_mem_hint_t heap_hint = { MEM_HINT_HEAP, heap_pages, 0 };
    wasmos_req_endpoint_t req_ep = { (uint32_t)strlen(req_ep_name), req_ep_rights };
    wasmos_cap_request_t cap = { (uint32_t)strlen(cap_name), cap_flags };

    int ok = 1;
    ok &= fwrite(&hdr, sizeof(hdr), 1, out) == 1;
    ok &= fwrite(name, 1, hdr.name_len, out) == hdr.name_len;
    ok &= fwrite(entry, 1, hdr.entry_len, out) == hdr.entry_len;
    if (has_req_ep) {
        ok &= fwrite(&req_ep, sizeof(req_ep), 1, out) == 1;
        ok &= fwrite(req_ep_name, 1, req_ep.name_len, out) == req_ep.name_len;
    }
    if (has_cap) {
        ok &= fwrite(&cap, sizeof(cap), 1, out) == 1;
        ok &= fwrite(cap_name, 1, cap.name_len, out) == cap.name_len;
    }
    ok &= fwrite(&stack_hint, sizeof(stack_hint), 1, out) == 1;
    ok &= fwrite(&heap_hint, sizeof(heap_hint), 1, out) == 1;
    ok &= fwrite(wasm, 1, (size_t)in_size, out) == (size_t)in_size;

    free(wasm);
    fclose(out);
    return ok ? 0 : 1;
}
