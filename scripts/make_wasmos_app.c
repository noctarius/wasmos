#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAGIC "WASMOSAP"
#define VERSION 3u
#define FLAG_DRIVER (1u << 0)
#define FLAG_SERVICE (1u << 1)
#define FLAG_APP (1u << 2)
#define FLAG_NEEDS_PRIV (1u << 3)

#define MEM_HINT_STACK 1u
#define MEM_HINT_HEAP 2u
#define MATCH_ANY_U8 0xFFu
#define MATCH_ANY_U16 0xFFFFu

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
    uint32_t entry_arg_binding_count;
    uint32_t mem_hint_count;
    uint8_t driver_match_class;
    uint8_t driver_match_subclass;
    uint8_t driver_match_prog_if;
    uint8_t driver_match_reserved0;
    uint16_t driver_match_vendor_id;
    uint16_t driver_match_device_id;
    uint16_t driver_io_port_min;
    uint16_t driver_io_port_max;
    uint32_t driver_match_count;
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
    uint32_t name_len;
} wasmos_entry_arg_binding_t;

typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t min_pages;
    uint32_t max_pages;
} wasmos_mem_hint_t;

typedef struct __attribute__((packed)) {
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t reserved0;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint32_t priority;
} wasmos_driver_match_t;

static int parse_u32(const char *s, uint32_t *out) {
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (!s || !*s || (end && *end != '\0') || v > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static char *
trim(char *s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

static void
strip_quotes(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int parse_u16_any(const char *s, uint16_t *out, uint16_t any_value) {
    if (!s || !out) {
        return -1;
    }
    if (strcmp(s, "any") == 0) {
        *out = any_value;
        return 0;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!*s || (end && *end != '\0') || v > 0xFFFFUL) {
        return -1;
    }
    *out = (uint16_t)v;
    return 0;
}

static int capability_name_supported(const char *name) {
    if (!name) {
        return 0;
    }
    return strcmp(name, "ipc.basic") == 0 ||
           strcmp(name, "io.port") == 0 ||
           strcmp(name, "irq.route") == 0 ||
           strcmp(name, "mmio.map") == 0 ||
           strcmp(name, "dma.buffer") == 0 ||
           strcmp(name, "system.control") == 0;
}

typedef struct {
    char name[64];
    uint32_t flags;
} manifest_cap_t;

typedef struct {
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t io_port_min;
    uint16_t io_port_max;
    uint32_t priority;
} manifest_match_t;

typedef struct {
    char name[64];
    char entry[64];
    char kind[16];
    uint8_t native;
    uint8_t storage_bootstrap;
    uint32_t stack_pages;
    uint32_t heap_pages;
    char req_ep_name[64];
    uint32_t req_ep_rights;
    char entry_arg_bindings[4][64];
    uint32_t entry_arg_binding_count;
    manifest_cap_t caps[8];
    uint32_t cap_count;
    manifest_match_t matches[8];
    uint32_t match_count;
} linker_manifest_t;

static int
parse_u32_auto(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (!s || !*s || (end && *end != '\0') || v > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int
manifest_parse_bool(const char *s, uint8_t *out)
{
    if (strcmp(s, "true") == 0) {
        *out = 1;
        return 0;
    }
    if (strcmp(s, "false") == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int
parse_linker_manifest(const char *path, linker_manifest_t *out)
{
    if (!path || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    strcpy(out->kind, "app");
    strcpy(out->req_ep_name, "-");
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    enum { SEC_NONE, SEC_PACKAGE, SEC_RESOURCES, SEC_IPC, SEC_CAP, SEC_MATCH } sec = SEC_NONE;
    int cap_idx = -1;
    int match_idx = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#') {
            continue;
        }
        char *hash = strchr(s, '#');
        if (hash) {
            *hash = '\0';
            s = trim(s);
            if (*s == '\0') {
                continue;
            }
        }
        if (strcmp(s, "[package]") == 0) { sec = SEC_PACKAGE; continue; }
        if (strcmp(s, "[resources]") == 0) { sec = SEC_RESOURCES; continue; }
        if (strcmp(s, "[ipc]") == 0) { sec = SEC_IPC; continue; }
        if (strcmp(s, "[[capabilities]]") == 0) {
            sec = SEC_CAP;
            if (out->cap_count >= 8) { fclose(f); return -1; }
            cap_idx = (int)out->cap_count++;
            memset(&out->caps[cap_idx], 0, sizeof(out->caps[cap_idx]));
            continue;
        }
        if (strcmp(s, "[[matches]]") == 0) {
            sec = SEC_MATCH;
            if (out->match_count >= 8) { fclose(f); return -1; }
            match_idx = (int)out->match_count++;
            out->matches[match_idx].class_code = MATCH_ANY_U8;
            out->matches[match_idx].subclass = MATCH_ANY_U8;
            out->matches[match_idx].prog_if = MATCH_ANY_U8;
            out->matches[match_idx].vendor_id = MATCH_ANY_U16;
            out->matches[match_idx].device_id = MATCH_ANY_U16;
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        strip_quotes(val);

        if (sec == SEC_PACKAGE) {
            if (strcmp(key, "name") == 0) {
                snprintf(out->name, sizeof(out->name), "%s", val);
            } else if (strcmp(key, "entry") == 0) {
                snprintf(out->entry, sizeof(out->entry), "%s", val);
            } else if (strcmp(key, "kind") == 0) {
                snprintf(out->kind, sizeof(out->kind), "%s", val);
            } else if (strcmp(key, "native") == 0) {
                if (manifest_parse_bool(val, &out->native) != 0) { fclose(f); return -1; }
            } else if (strcmp(key, "storage_bootstrap") == 0) {
                if (manifest_parse_bool(val, &out->storage_bootstrap) != 0) { fclose(f); return -1; }
            }
        } else if (sec == SEC_RESOURCES) {
            if (strcmp(key, "stack_pages") == 0) {
                if (parse_u32_auto(val, &out->stack_pages) != 0) { fclose(f); return -1; }
            } else if (strcmp(key, "heap_pages") == 0) {
                if (parse_u32_auto(val, &out->heap_pages) != 0) { fclose(f); return -1; }
            }
        } else if (sec == SEC_IPC) {
            if (strcmp(key, "required_endpoint_name") == 0) {
                snprintf(out->req_ep_name, sizeof(out->req_ep_name), "%s", val);
            } else if (strcmp(key, "required_endpoint_rights") == 0) {
                if (parse_u32_auto(val, &out->req_ep_rights) != 0) { fclose(f); return -1; }
            } else if (strcmp(key, "entry_arg_bindings") == 0) {
                if (*val != '[') { fclose(f); return -1; }
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%s", val);
                char *p = tmp;
                if (*p == '[') p++;
                char *rbr = strrchr(p, ']');
                if (rbr) *rbr = '\0';
                out->entry_arg_binding_count = 0;
                char *tok = strtok(p, ",");
                while (tok) {
                    tok = trim(tok);
                    strip_quotes(tok);
                    if (*tok) {
                        if (out->entry_arg_binding_count >= 4) { fclose(f); return -1; }
                        snprintf(out->entry_arg_bindings[out->entry_arg_binding_count],
                                 sizeof(out->entry_arg_bindings[0]), "%s", tok);
                        out->entry_arg_binding_count++;
                    }
                    tok = strtok(NULL, ",");
                }
            }
        } else if (sec == SEC_CAP && cap_idx >= 0) {
            if (strcmp(key, "name") == 0) {
                snprintf(out->caps[cap_idx].name, sizeof(out->caps[cap_idx].name), "%s", val);
            } else if (strcmp(key, "flags") == 0) {
                if (parse_u32_auto(val, &out->caps[cap_idx].flags) != 0) { fclose(f); return -1; }
            }
        } else if (sec == SEC_MATCH && match_idx >= 0) {
            manifest_match_t *m = &out->matches[match_idx];
            uint16_t u16 = 0;
            uint32_t u32 = 0;
            if (strcmp(key, "bus") == 0) {
                if (strcmp(val, "pci") != 0) { fclose(f); return -1; }
            } else if (strcmp(key, "class") == 0) {
                if (parse_u16_any(val, &u16, MATCH_ANY_U8) != 0) { fclose(f); return -1; }
                m->class_code = (uint8_t)u16;
            } else if (strcmp(key, "subclass") == 0) {
                if (parse_u16_any(val, &u16, MATCH_ANY_U8) != 0) { fclose(f); return -1; }
                m->subclass = (uint8_t)u16;
            } else if (strcmp(key, "prog_if") == 0) {
                if (parse_u16_any(val, &u16, MATCH_ANY_U8) != 0) { fclose(f); return -1; }
                m->prog_if = (uint8_t)u16;
            } else if (strcmp(key, "vendor") == 0) {
                if (parse_u16_any(val, &u16, MATCH_ANY_U16) != 0) { fclose(f); return -1; }
                m->vendor_id = u16;
            } else if (strcmp(key, "device") == 0) {
                if (parse_u16_any(val, &u16, MATCH_ANY_U16) != 0) { fclose(f); return -1; }
                m->device_id = u16;
            } else if (strcmp(key, "io_port_min") == 0) {
                if (parse_u16_any(val, &u16, 0) != 0) { fclose(f); return -1; }
                m->io_port_min = u16;
            } else if (strcmp(key, "io_port_max") == 0) {
                if (parse_u16_any(val, &u16, 0) != 0) { fclose(f); return -1; }
                m->io_port_max = u16;
            } else if (strcmp(key, "priority") == 0) {
                if (parse_u32_auto(val, &u32) != 0) { fclose(f); return -1; }
                m->priority = u32;
            }
        }
    }
    fclose(f);
    if (out->name[0] == '\0' || out->entry[0] == '\0') {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 6 && strcmp(argv[1], "--manifest") == 0) {
        const char *manifest_path = argv[2];
        const char *in_path = NULL;
        const char *out_path = NULL;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--in") == 0) {
                in_path = argv[i + 1];
            } else if (strcmp(argv[i], "--out") == 0) {
                out_path = argv[i + 1];
            } else {
                fprintf(stderr, "unknown flag '%s'\n", argv[i]);
                return 1;
            }
        }
        if (!in_path || !out_path) {
            fprintf(stderr, "usage: %s --manifest <path> --in <in.wasm|elf> --out <out.wap>\n", argv[0]);
            return 1;
        }
        linker_manifest_t lm;
        if (parse_linker_manifest(manifest_path, &lm) != 0) {
            fprintf(stderr, "failed to parse linker manifest: %s\n", manifest_path);
            return 1;
        }
        uint32_t flags = 0;
        if (strcmp(lm.kind, "driver") == 0) flags |= FLAG_DRIVER;
        else if (strcmp(lm.kind, "service") == 0) flags |= FLAG_SERVICE;
        else flags |= FLAG_APP;
        if (lm.native) flags |= (1u << 4);
        if (lm.storage_bootstrap) flags |= (1u << 5);

        uint32_t cap_count = lm.cap_count;
        const char *cap_names[8];
        wasmos_cap_request_t caps[8];
        for (uint32_t i = 0; i < cap_count; ++i) {
            if (!capability_name_supported(lm.caps[i].name)) {
                fprintf(stderr, "unknown capability '%s'\n", lm.caps[i].name);
                return 1;
            }
            cap_names[i] = lm.caps[i].name;
            caps[i].name_len = (uint32_t)strlen(lm.caps[i].name);
            caps[i].flags = lm.caps[i].flags;
        }

        wasmos_driver_match_t driver_matches[8];
        uint32_t driver_match_count = lm.match_count;
        for (uint32_t i = 0; i < driver_match_count; ++i) {
            driver_matches[i].class_code = lm.matches[i].class_code;
            driver_matches[i].subclass = lm.matches[i].subclass;
            driver_matches[i].prog_if = lm.matches[i].prog_if;
            driver_matches[i].reserved0 = 0;
            driver_matches[i].vendor_id = lm.matches[i].vendor_id;
            driver_matches[i].device_id = lm.matches[i].device_id;
            driver_matches[i].io_port_min = lm.matches[i].io_port_min;
            driver_matches[i].io_port_max = lm.matches[i].io_port_max;
            driver_matches[i].priority = lm.matches[i].priority;
        }

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

        FILE *outf = fopen(out_path, "wb");
        if (!outf) {
            perror("open output");
            free(wasm);
            return 1;
        }

        wasmos_app_header_t hdr;
        memcpy(hdr.magic, MAGIC, 8);
        hdr.version = VERSION;
        hdr.header_size = sizeof(hdr);
        hdr.flags = flags;
        hdr.name_len = (uint32_t)strlen(lm.name);
        hdr.entry_len = (uint32_t)strlen(lm.entry);
        hdr.wasm_size = (uint32_t)in_size;
        hdr.req_ep_count = (lm.req_ep_name[0] == '-' && lm.req_ep_name[1] == '\0') ? 0u : 1u;
        hdr.cap_count = cap_count;
        hdr.entry_arg_binding_count = lm.entry_arg_binding_count;
        hdr.mem_hint_count = 2;
        hdr.driver_match_class = (driver_match_count > 0) ? driver_matches[0].class_code : MATCH_ANY_U8;
        hdr.driver_match_subclass = (driver_match_count > 0) ? driver_matches[0].subclass : MATCH_ANY_U8;
        hdr.driver_match_prog_if = (driver_match_count > 0) ? driver_matches[0].prog_if : MATCH_ANY_U8;
        hdr.driver_match_reserved0 = 0;
        hdr.driver_match_vendor_id = (driver_match_count > 0) ? driver_matches[0].vendor_id : MATCH_ANY_U16;
        hdr.driver_match_device_id = (driver_match_count > 0) ? driver_matches[0].device_id : MATCH_ANY_U16;
        hdr.driver_io_port_min = (driver_match_count > 0) ? driver_matches[0].io_port_min : 0;
        hdr.driver_io_port_max = (driver_match_count > 0) ? driver_matches[0].io_port_max : 0;
        hdr.driver_match_count = driver_match_count;
        hdr.reserved = 0;

        wasmos_mem_hint_t stack_hint = { MEM_HINT_STACK, lm.stack_pages, 0 };
        wasmos_mem_hint_t heap_hint = { MEM_HINT_HEAP, lm.heap_pages, 0 };
        wasmos_req_endpoint_t req_ep = { (uint32_t)strlen(lm.req_ep_name), lm.req_ep_rights };
        wasmos_entry_arg_binding_t entry_arg_binding_hdrs[4];

        int ok = 1;
        ok &= fwrite(&hdr, sizeof(hdr), 1, outf) == 1;
        ok &= fwrite(lm.name, 1, hdr.name_len, outf) == hdr.name_len;
        ok &= fwrite(lm.entry, 1, hdr.entry_len, outf) == hdr.entry_len;
        if (hdr.req_ep_count == 1u) {
            ok &= fwrite(&req_ep, sizeof(req_ep), 1, outf) == 1;
            ok &= fwrite(lm.req_ep_name, 1, req_ep.name_len, outf) == req_ep.name_len;
        }
        for (uint32_t i = 0; i < cap_count; ++i) {
            ok &= fwrite(&caps[i], sizeof(caps[i]), 1, outf) == 1;
            ok &= fwrite(cap_names[i], 1, caps[i].name_len, outf) == caps[i].name_len;
        }
        for (uint32_t i = 0; i < lm.entry_arg_binding_count; ++i) {
            entry_arg_binding_hdrs[i].name_len = (uint32_t)strlen(lm.entry_arg_bindings[i]);
            ok &= fwrite(&entry_arg_binding_hdrs[i], sizeof(entry_arg_binding_hdrs[i]), 1, outf) == 1;
            ok &= fwrite(lm.entry_arg_bindings[i], 1, entry_arg_binding_hdrs[i].name_len, outf) ==
                  entry_arg_binding_hdrs[i].name_len;
        }
        for (uint32_t i = 0; i < driver_match_count; ++i) {
            ok &= fwrite(&driver_matches[i], sizeof(driver_matches[i]), 1, outf) == 1;
        }
        ok &= fwrite(&stack_hint, sizeof(stack_hint), 1, outf) == 1;
        ok &= fwrite(&heap_hint, sizeof(heap_hint), 1, outf) == 1;
        ok &= fwrite(wasm, 1, (size_t)in_size, outf) == (size_t)in_size;

        free(wasm);
        fclose(outf);
        return ok ? 0 : 1;
    }
    if (argc < 11) {
        fprintf(stderr, "usage: %s <in.wasm> <out.wap> <name> <entry> <stack_pages> <heap_pages> <flags> <req_ep_name|- > <req_ep_rights> <cap_count> [<cap_name> <cap_flags>]...\n", argv[0]);
        fprintf(stderr, "legacy: %s <in.wasm> <out.wap> <name> <entry> <stack_pages> <heap_pages> <flags> <req_ep_name|- > <req_ep_rights> <cap_name|- > <cap_flags>\n", argv[0]);
        return 1;
    }

    const char *in_path = argv[1];
    const char *out_path = argv[2];
    const char *name = argv[3];
    const char *entry = argv[4];
    const char *req_ep_name = argv[8];
    uint32_t stack_pages = 0;
    uint32_t heap_pages = 0;
    uint32_t flags = 0;
    uint32_t req_ep_rights = 0;
    uint32_t cap_count = 0;
    const uint32_t cap_max = 8;
    const char *cap_names[8];
    wasmos_cap_request_t caps[8];
    uint32_t entry_arg_binding_count = 0;
    const uint32_t entry_arg_binding_max = 4;
    const char *entry_arg_bindings[4];
    uint8_t driver_match_class = MATCH_ANY_U8;
    uint8_t driver_match_subclass = MATCH_ANY_U8;
    uint8_t driver_match_prog_if = MATCH_ANY_U8;
    uint16_t driver_match_vendor_id = MATCH_ANY_U16;
    uint16_t driver_match_device_id = MATCH_ANY_U16;
    uint16_t driver_io_port_min = 0;
    uint16_t driver_io_port_max = 0;
    uint32_t driver_match_count = 0;
    if (parse_u32(argv[5], &stack_pages) != 0 || parse_u32(argv[6], &heap_pages) != 0) {
        fprintf(stderr, "invalid stack/heap page value\n");
        return 1;
    }
    if (parse_u32(argv[7], &flags) != 0 ||
        parse_u32(argv[9], &req_ep_rights) != 0) {
        fprintf(stderr, "invalid flags/req_ep_rights value\n");
        return 1;
    }
    int has_req_ep = !(req_ep_name[0] == '-' && req_ep_name[1] == '\0');

    int legacy_mode = 0;
    if (parse_u32(argv[10], &cap_count) == 0) {
        int min_tail = (int)(11u + (cap_count * 2u));
        if (argc < min_tail) {
            fprintf(stderr, "invalid capability argument layout\n");
            return 1;
        }
        if (cap_count > cap_max) {
            fprintf(stderr, "cap_count exceeds max supported entries (%u)\n", cap_max);
            return 1;
        }
        for (uint32_t i = 0; i < cap_count; ++i) {
            const char *cap_name = argv[11 + (i * 2u)];
            uint32_t cap_flags = 0;
            if (!cap_name || cap_name[0] == '\0' ||
                (cap_name[0] == '-' && cap_name[1] == '\0') ||
                parse_u32(argv[12 + (i * 2u)], &cap_flags) != 0) {
                fprintf(stderr, "invalid capability entry at index %u\n", i);
                return 1;
            }
            if (!capability_name_supported(cap_name)) {
                fprintf(stderr, "unknown capability '%s' at index %u\n", cap_name, i);
                return 1;
            }
            if (cap_flags != 0) {
                fprintf(stderr, "unsupported capability flags for '%s' at index %u\n", cap_name, i);
                return 1;
            }
            cap_names[i] = cap_name;
            caps[i].name_len = (uint32_t)strlen(cap_name);
            caps[i].flags = cap_flags;
        }
        int next = min_tail;
        if (argc > next) {
            while (next < argc) {
                if (strcmp(argv[next], "--entry-arg-bindings") == 0) {
                    next++;
                    if (next >= argc || parse_u32(argv[next], &entry_arg_binding_count) != 0) {
                        fprintf(stderr, "invalid entry arg binding count\n");
                        return 1;
                    }
                    next++;
                    if (entry_arg_binding_count > entry_arg_binding_max ||
                        argc < next + (int)entry_arg_binding_count) {
                        fprintf(stderr, "invalid entry arg bindings layout\n");
                        return 1;
                    }
                    for (uint32_t i = 0; i < entry_arg_binding_count; ++i) {
                        const char *binding = argv[next + (int)i];
                        if (!binding || binding[0] == '\0') {
                            fprintf(stderr, "invalid entry arg binding at index %u\n", i);
                            return 1;
                        }
                        entry_arg_bindings[i] = binding;
                    }
                    next += (int)entry_arg_binding_count;
                } else if (strcmp(argv[next], "--driver-match") == 0) {
                    if (next + 7 >= argc) {
                        fprintf(stderr, "invalid --driver-match layout\n");
                        return 1;
                    }
                    uint16_t cls = 0;
                    uint16_t sub = 0;
                    uint16_t prog = 0;
                    if (parse_u16_any(argv[next + 1], &cls, MATCH_ANY_U8) != 0 ||
                        parse_u16_any(argv[next + 2], &sub, MATCH_ANY_U8) != 0 ||
                        parse_u16_any(argv[next + 3], &prog, MATCH_ANY_U8) != 0 ||
                        parse_u16_any(argv[next + 4], &driver_match_vendor_id, MATCH_ANY_U16) != 0 ||
                        parse_u16_any(argv[next + 5], &driver_match_device_id, MATCH_ANY_U16) != 0 ||
                        parse_u16_any(argv[next + 6], &driver_io_port_min, 0) != 0 ||
                        parse_u16_any(argv[next + 7], &driver_io_port_max, 0) != 0) {
                        fprintf(stderr, "invalid --driver-match value\n");
                        return 1;
                    }
                    driver_match_class = (uint8_t)cls;
                    driver_match_subclass = (uint8_t)sub;
                    driver_match_prog_if = (uint8_t)prog;
                    driver_match_count = 1;
                    next += 8;
                } else {
                    fprintf(stderr, "unknown trailing argument '%s'\n", argv[next]);
                    return 1;
                }
            }
        }
    } else if (argc == 12) {
        /* Backward compatibility mode: one optional capability pair. */
        legacy_mode = 1;
        const char *cap_name = argv[10];
        uint32_t cap_flags = 0;
        if (parse_u32(argv[11], &cap_flags) != 0) {
            fprintf(stderr, "invalid legacy cap_flags value\n");
            return 1;
        }
        if (!(cap_name[0] == '-' && cap_name[1] == '\0')) {
            if (!capability_name_supported(cap_name)) {
                fprintf(stderr, "unknown legacy capability '%s'\n", cap_name);
                return 1;
            }
            if (cap_flags != 0) {
                fprintf(stderr, "unsupported legacy capability flags for '%s'\n", cap_name);
                return 1;
            }
            cap_count = 1;
            cap_names[0] = cap_name;
            caps[0].name_len = (uint32_t)strlen(cap_name);
            caps[0].flags = cap_flags;
        }
    } else {
        fprintf(stderr, "invalid capability argument layout\n");
        if (!legacy_mode) {
            fprintf(stderr, "usage: %s <in.wasm> <out.wap> <name> <entry> <stack_pages> <heap_pages> <flags> <req_ep_name|- > <req_ep_rights> <cap_count> [<cap_name> <cap_flags>]...\n", argv[0]);
        }
        return 1;
    }

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
    hdr.cap_count = cap_count;
    hdr.entry_arg_binding_count = entry_arg_binding_count;
    hdr.mem_hint_count = 2;
    hdr.driver_match_class = driver_match_class;
    hdr.driver_match_subclass = driver_match_subclass;
    hdr.driver_match_prog_if = driver_match_prog_if;
    hdr.driver_match_reserved0 = 0;
    hdr.driver_match_vendor_id = driver_match_vendor_id;
    hdr.driver_match_device_id = driver_match_device_id;
    hdr.driver_io_port_min = driver_io_port_min;
    hdr.driver_io_port_max = driver_io_port_max;
    hdr.driver_match_count = driver_match_count;
    hdr.reserved = 0;

    wasmos_mem_hint_t stack_hint = { MEM_HINT_STACK, stack_pages, 0 };
    wasmos_mem_hint_t heap_hint = { MEM_HINT_HEAP, heap_pages, 0 };
    wasmos_req_endpoint_t req_ep = { (uint32_t)strlen(req_ep_name), req_ep_rights };
    wasmos_entry_arg_binding_t entry_arg_binding_hdrs[4];

    int ok = 1;
    ok &= fwrite(&hdr, sizeof(hdr), 1, out) == 1;
    ok &= fwrite(name, 1, hdr.name_len, out) == hdr.name_len;
    ok &= fwrite(entry, 1, hdr.entry_len, out) == hdr.entry_len;
    if (has_req_ep) {
        ok &= fwrite(&req_ep, sizeof(req_ep), 1, out) == 1;
        ok &= fwrite(req_ep_name, 1, req_ep.name_len, out) == req_ep.name_len;
    }
    for (uint32_t i = 0; i < cap_count; ++i) {
        ok &= fwrite(&caps[i], sizeof(caps[i]), 1, out) == 1;
        ok &= fwrite(cap_names[i], 1, caps[i].name_len, out) == caps[i].name_len;
    }
    for (uint32_t i = 0; i < entry_arg_binding_count; ++i) {
        entry_arg_binding_hdrs[i].name_len = (uint32_t)strlen(entry_arg_bindings[i]);
        ok &= fwrite(&entry_arg_binding_hdrs[i], sizeof(entry_arg_binding_hdrs[i]), 1, out) == 1;
        ok &= fwrite(entry_arg_bindings[i], 1, entry_arg_binding_hdrs[i].name_len, out) ==
              entry_arg_binding_hdrs[i].name_len;
    }
    if (driver_match_count > 0) {
        wasmos_driver_match_t dm;
        dm.class_code = driver_match_class;
        dm.subclass = driver_match_subclass;
        dm.prog_if = driver_match_prog_if;
        dm.reserved0 = 0;
        dm.vendor_id = driver_match_vendor_id;
        dm.device_id = driver_match_device_id;
        dm.io_port_min = driver_io_port_min;
        dm.io_port_max = driver_io_port_max;
        dm.priority = 0;
        ok &= fwrite(&dm, sizeof(dm), 1, out) == 1;
    }
    ok &= fwrite(&stack_hint, sizeof(stack_hint), 1, out) == 1;
    ok &= fwrite(&heap_hint, sizeof(heap_hint), 1, out) == 1;
    ok &= fwrite(wasm, 1, (size_t)in_size, out) == (size_t)in_size;

    free(wasm);
    fclose(out);
    return ok ? 0 : 1;
}
