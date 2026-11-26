#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Container identity. MAGIC is the 8-byte signature, compared byte-for-byte and
 * NOT NUL-terminated in the file. VERSION selects which header layout the parser
 * applies; this packer emits 10, and src/kernel/wasmos_app.c accepts only 10. */
#define MAGIC "WASMOSAP"
#define VERSION 10u

/* Header flags, mirroring WASMOS_APP_FLAG_* in src/kernel/include/wasmos_app.h.
 * DRIVER/SERVICE/APP are the package kind and exactly one is set, derived from
 * the manifest's `kind`. NEEDS_PRIV (1<<3) is defined for parity with the kernel
 * but never set by this tool. Three further bits have no name here and are
 * written as literals in main(): 1<<4 NATIVE (the payload is an ELF, not WASM;
 * the parser rejects it unless DRIVER or SERVICE is also set), 1<<5
 * STORAGE_BOOTSTRAP, 1<<6 WANTS_TTY. */
#define FLAG_DRIVER (1u << 0)
#define FLAG_SERVICE (1u << 1)
#define FLAG_APP (1u << 2)
#define FLAG_NEEDS_PRIV (1u << 3)

/* Capacity of the header's subsystem_tag field, in bytes. A tag shorter than
 * this is NUL-padded; one of exactly this length is not terminated. */
#define SUBSYSTEM_TAG_LEN 8u

/* wasmos_mem_hint_t.kind values, a subset of WASMOS_APP_MEM_HINT_* in
 * src/kernel/include/wasmos_app.h. Only these two are emitted, always as a pair
 * and always in this order. */
#define MEM_HINT_STACK 1u
#define MEM_HINT_HEAP 2u
/* Region kinds mirror src/drivers/include/wasmos_driver_abi.h and the bound
 * mirrors src/kernel/include/wasmos_app.h. The packer builds for the host and
 * cannot include either, so a change on those sides has to be repeated here or
 * the kernel parser rejects the package. */
#define WASMOS_APP_REGION_IO 0u
#define WASMOS_APP_REGION_BAR 1u
#define WASMOS_APP_MAX_REGIONS 4u
/* Dependency records, mirroring WASMOS_APP_DEP_* and WASMOS_APP_MAX_DEPS in
 * src/kernel/include/wasmos_app.h (see the [deps] contract there). The name
 * limits are the registry's: WASMOS_SVC_NAME_MAX / WASMOS_SVC_CLASS_MAX minus the
 * terminator. */
#define DEP_NEEDS 1u
#define DEP_WANTS 2u
#define DEP_PROVIDES 3u
#define DEP_KIND_NAME 1u
#define DEP_KIND_CLASS 2u
#define DEP_NAME_MAX 35u
#define DEP_CLASS_MAX 15u
#define MAX_DEPS 16u

/* Fixed head of a .wap package. It must stay byte-identical to
 * wasmos_app_header_t in src/kernel/wasmos_app.c, which rejects the package
 * unless header_size equals its own sizeof.
 *
 * A package is this header followed by variable-length sections in exactly the
 * order wasmos_app_parse() walks them:
 *   header, name, entry, dep_count x (wasmos_dep_record_t + name bytes),
 *   cap_count x (wasmos_cap_request_t + name bytes),
 *   region_count x wasmos_region_entry_t,
 *   mem_hint_count x wasmos_mem_hint_t (stack then heap),
 *   wasm_size raw payload bytes, then compiled_size WARP AOT bytes if non-zero.
 * Nothing is padded or aligned between sections; the parser advances by the
 * counts in this header alone.
 *
 * Fields, all little-endian (the format is not byte-order portable):
 *   magic[8]                 "WASMOSAP", not NUL-terminated.
 *   version                  Always 10 from this packer.
 *   header_size              sizeof(this struct); the parser compares it against
 *                            its own and also uses it as the offset of the first
 *                            section, so name bytes start exactly here.
 *   flags                    FLAG_* bitmask; see the flag definitions above.
 *   name_len / entry_len     Byte lengths of the name and entry-symbol strings
 *                            that follow the header, in that order. Neither is
 *                            NUL-terminated and neither length includes a
 *                            terminator. Both must be non-zero.
 *   wasm_size                Byte length of the payload, which is WASM bytecode
 *                            unless the NATIVE flag is set, in which case it is
 *                            an ELF image.
 *   dep_count                Number of dependency records (max MAX_DEPS), from
 *                            the manifest's [deps] table; 0 without one.
 *   cap_count                Number of capability-request records (max 8).
 *   mem_hint_count           Number of memory-hint records; always 2.
 *   compiled_size            Byte length of the WARP AOT binary appended after
 *                            the payload; 0 when absent.
 *   subsystem_tag            Up-to-8-byte tag, NUL-padded, from [A-Z0-9+_-];
 *                            defaults to "NATIVE" or "WASM".
 *   region_count             Number of wasmos_region_entry_t records, written
 *                            after the capability table and before the mem hints.
 */
typedef struct __attribute__((packed)) {
    char magic[8];
    uint16_t version;
    uint16_t header_size;
    uint32_t flags;
    uint32_t name_len;
    uint32_t entry_len;
    uint32_t wasm_size;
    uint32_t dep_count;
    uint32_t cap_count;
    uint32_t mem_hint_count;
    uint32_t compiled_size; /* size of the WARP AOT binary appended after WASM; 0 if absent */
    char subsystem_tag[SUBSYSTEM_TAG_LEN];
    uint32_t region_count; /* declared register windows */
} wasmos_app_header_t;

/* One declared register window, carrying no trailing name. Its position in the
 * region section IS the region index the driver addresses at runtime.
 *   kind       WASMOS_APP_REGION_IO or _BAR.
 *   bar_index  BAR number 0..5, read only when kind is _BAR.
 *   first/last Inclusive I/O port range, read only when kind is _IO. The parser
 *              rejects first > last; a single port is expressed as first==last. */
typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint8_t bar_index;
    uint16_t first;
    uint16_t last;
} wasmos_region_entry_t;

/* Dependency record, followed immediately by name_len raw name bytes with no
 * terminator. */
typedef struct __attribute__((packed)) {
    uint8_t strength; /* DEP_NEEDS / DEP_WANTS / DEP_PROVIDES */
    uint8_t kind;     /* DEP_KIND_NAME / DEP_KIND_CLASS */
    uint16_t name_len;
} wasmos_dep_record_t;

/* Capability-request record, followed immediately by name_len raw name bytes
 * with no terminator. The name must be one capability_name_supported() accepts,
 * and flags is required to be 0 on the positional command line. */
typedef struct __attribute__((packed)) {
    uint32_t name_len;
    uint32_t flags;
} wasmos_cap_request_t;

/* Memory-hint record. kind is a MEM_HINT_* value and min_pages a count of 4 KiB
 * pages, which the launcher multiplies out into a byte size. The parser reads
 * min_pages only; max_pages is written as 0 and ignored. A hint of 0 pages means
 * "unspecified" and the launcher substitutes its own 64 KiB default, so it does
 * not request a zero-sized stack or heap. */
typedef struct __attribute__((packed)) {
    uint32_t kind;
    uint32_t min_pages;
    uint32_t max_pages;
} wasmos_mem_hint_t;

static int capability_name_supported(const char* name) {
    if (!name) {
        return 0;
    }
    return strcmp(name, "ipc.basic") == 0 || strcmp(name, "io.port") == 0 ||
           strcmp(name, "irq.route") == 0 || strcmp(name, "mmio.map") == 0 ||
           strcmp(name, "dma.buffer") == 0 || strcmp(name, "system.control") == 0 ||
           strcmp(name, "subsystem.register") == 0 || strcmp(name, "svc.class") == 0 ||
           strcmp(name, "console.owner") == 0 || strcmp(name, "spawn.grant") == 0 ||
           strcmp(name, "svc.control") == 0 || strcmp(name, "pci.config") == 0 ||
           strcmp(name, "rtc.efi") == 0 || strcmp(name, "inventory.device") == 0 ||
           strcmp(name, "inventory.storage") == 0;
}

static int subsystem_tag_has_valid_char(char c) {
    return ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || c == '+' || c == '_' ||
           c == '-';
}

static int subsystem_tag_valid(const char* tag) {
    size_t len = 0;
    if (!tag || !tag[0]) {
        return 0;
    }
    for (; tag[len] != '\0'; ++len) {
        if (len >= SUBSYSTEM_TAG_LEN || !subsystem_tag_has_valid_char(tag[len])) {
            return 0;
        }
    }
    return len > 0 && len <= SUBSYSTEM_TAG_LEN;
}

static void subsystem_tag_copy(char dst[SUBSYSTEM_TAG_LEN], const char* src) {
    memset(dst, 0, SUBSYSTEM_TAG_LEN);
    if (!src) {
        return;
    }
    for (size_t i = 0; i < SUBSYSTEM_TAG_LEN && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
}

typedef struct {
    char name[64];
    uint32_t flags;
} manifest_cap_t;

/* One register window the driver declares it needs. A driver names windows, not
 * ports: `io` is a fixed range it knows statically (legacy/ISA-compat), `bar` is
 * wherever firmware put a BAR of the matched device. Declaration order is the
 * region index the driver addresses at runtime. */
typedef struct {
    uint8_t kind; /* WASMOS_APP_REGION_* */
    uint8_t bar_index;
    uint16_t first;
    uint16_t last;
} manifest_region_t;

/* One [deps] entry. `name` is NUL-terminated here and written without the
 * terminator. */
typedef struct {
    uint8_t strength; /* DEP_* */
    uint8_t kind;     /* DEP_KIND_* */
    char name[DEP_NAME_MAX + 1];
} manifest_dep_t;

/* Parsed form of a linker manifest: the packer's whole input apart from the
 * payload bytes. Fixed-capacity throughout, and parse_linker_manifest() fails
 * rather than truncating when a table overflows. `kind` is the manifest's kind
 * string ("driver"/"service"/anything else meaning app) and is mapped to a
 * FLAG_* bit in main(). Strings are NUL-terminated here but are written to the package as raw bytes
 * with an explicit length. */
typedef struct {
    char name[64];
    char entry[64];
    char kind[16];
    char subsystem[SUBSYSTEM_TAG_LEN + 1];
    uint8_t native;
    uint8_t storage_bootstrap;
    uint8_t wants_tty;
    uint32_t stack_pages;
    uint32_t heap_pages;
    manifest_dep_t deps[MAX_DEPS];
    uint32_t dep_count;
    manifest_cap_t caps[8];
    uint32_t cap_count;
    manifest_region_t regions[WASMOS_APP_MAX_REGIONS];
    uint32_t region_count;
} linker_manifest_t;

static int parse_u32_auto(const char* s, uint32_t* out) {
    if (!s || !out || !*s) {
        return -1;
    }
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if ((end && *end != '\0') || v > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static int manifest_parse_bool(const char* s, uint8_t* out) {
    if (!s || !out) {
        return -1;
    }
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

static int parse_u32(const char* s, uint32_t* out) {
    if (!s || !out || !*s) {
        return -1;
    }
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if ((end && *end != '\0') || v > 0xFFFFFFFFUL) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static char* trim(char* s) {
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char* end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return s;
}

static void strip_quotes(char* s) {
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

/* Parse one [deps] value, a single-line array of double-quoted strings such as
 * ["fs.vfs", "class:block"], appending each entry to out->deps with `strength`.
 * A "class:" prefix makes the entry a class; anything else is a service name.
 * Names use [a-z0-9._-] and must fit their kind's limit (DEP_NAME_MAX /
 * DEP_CLASS_MAX). Returns 0, or -1 with a message on stderr for a malformed
 * array, an invalid or duplicate entry, or more than MAX_DEPS entries in total. */
static int parse_dep_list(const char* path, const char* val, uint8_t strength,
                          linker_manifest_t* out) {
    const char* p = val;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '[') {
        fprintf(stderr, "%s: [deps] value must be an array of strings\n", path);
        return -1;
    }
    p++;
    for (;;) {
        while (isspace((unsigned char)*p) || *p == ',') {
            p++;
        }
        if (*p == ']') {
            p++;
            break;
        }
        if (*p != '"') {
            fprintf(stderr, "%s: [deps] entries must be double-quoted strings\n", path);
            return -1;
        }
        const char* start = ++p;
        while (*p && *p != '"') {
            p++;
        }
        if (*p != '"') {
            fprintf(stderr, "%s: unterminated [deps] entry\n", path);
            return -1;
        }
        size_t len = (size_t)(p - start);
        p++;
        uint8_t kind = DEP_KIND_NAME;
        size_t max_len = DEP_NAME_MAX;
        if (len > 6 && strncmp(start, "class:", 6) == 0) {
            kind = DEP_KIND_CLASS;
            max_len = DEP_CLASS_MAX;
            start += 6;
            len -= 6;
        }
        if (len == 0 || len > max_len) {
            fprintf(stderr,
                    "%s: [deps] entry '%.*s' is empty or longer than %zu\n",
                    path,
                    (int)len,
                    start,
                    max_len);
            return -1;
        }
        for (size_t i = 0; i < len; ++i) {
            char c = start[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                  c == '-')) {
                fprintf(stderr,
                        "%s: [deps] entry '%.*s' has a character outside [a-z0-9._-]\n",
                        path,
                        (int)len,
                        start);
                return -1;
            }
        }
        for (uint32_t i = 0; i < out->dep_count; ++i) {
            const manifest_dep_t* d = &out->deps[i];
            if (d->strength == strength && d->kind == kind && strlen(d->name) == len &&
                memcmp(d->name, start, len) == 0) {
                fprintf(stderr, "%s: duplicate [deps] entry '%.*s'\n", path, (int)len, start);
                return -1;
            }
        }
        if (out->dep_count >= MAX_DEPS) {
            fprintf(stderr, "%s: more than %u [deps] entries\n", path, MAX_DEPS);
            return -1;
        }
        manifest_dep_t* d = &out->deps[out->dep_count++];
        d->strength = strength;
        d->kind = kind;
        memcpy(d->name, start, len);
        d->name[len] = '\0';
    }
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0') {
        fprintf(stderr, "%s: trailing text after a [deps] array\n", path);
        return -1;
    }
    return 0;
}

/* Read a TOML-ish linker manifest into *out. The accepted grammar is a small
 * subset: '#' starts a comment to end of line, blank lines are skipped, section
 * headers are [package], [resources], [deps], [link] and the repeatable
 * [[capabilities]] and [[regions]], and everything else must be a key = value
 * line whose value may be double-quoted. Keys outside a known section, and
 * unknown keys inside one, are ignored silently. [deps] takes needs, wants and
 * provides, each a single-line array (parse_dep_list); an unknown key there is
 * an error, since a misspelt dependency would otherwise vanish. The [ipc] table
 * of .wap v9 and earlier is refused with a pointer to [deps]. [link] is recognized and wholly
 * ignored: it holds link-time memory sizes the toolchain driver reads, and
 * recognizing it keeps its keys from being read as keys of the section above.
 *
 * Defaults applied before parsing: kind "app" and no dependencies. Each
 * [[regions]] block starts as an I/O region with an empty
 * range. A missing
 * subsystem is filled in from the native flag as "NATIVE" or "WASM".
 *
 * Returns 0 on success, -1 on any failure: unreadable file, a malformed numeric
 * or boolean value, an unknown region kind, a table exceeding its capacity (8
 * capabilities, WASMOS_APP_MAX_REGIONS regions, MAX_DEPS dependencies), an
 * invalid [deps] entry, an [ipc] table, a missing name or entry, or an
 * invalid subsystem tag. *out is fully overwritten either way and holds partial data on
 * failure. */
static int parse_linker_manifest(const char* path, linker_manifest_t* out) {
    if (!path || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    strcpy(out->kind, "app");
    FILE* f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    enum {
        SEC_NONE,
        SEC_PACKAGE,
        SEC_RESOURCES,
        SEC_DEPS,
        SEC_LINK,
        SEC_CAP,
        SEC_REGION
    } sec = SEC_NONE;
    int region_idx = -1;
    int cap_idx = -1;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* s = trim(line);
        if (*s == '\0' || *s == '#') {
            continue;
        }
        char* hash = strchr(s, '#');
        if (hash) {
            *hash = '\0';
            s = trim(s);
            if (*s == '\0') {
                continue;
            }
        }
        if (strcmp(s, "[package]") == 0) {
            sec = SEC_PACKAGE;
            continue;
        }
        if (strcmp(s, "[resources]") == 0) {
            sec = SEC_RESOURCES;
            continue;
        }
        if (strcmp(s, "[deps]") == 0) {
            sec = SEC_DEPS;
            continue;
        }
        if (strcmp(s, "[ipc]") == 0) {
            fprintf(stderr,
                    "%s: the [ipc] table (required_endpoint_name) was removed in .wap v10; "
                    "declare peers in [deps] instead\n",
                    path);
            fclose(f);
            return -1;
        }
        /* [link] carries link-time memory sizes for the toolchain driver
         * (wasmos-clang) and nothing the container needs.  It is recognized so
         * that its keys are dropped here rather than read as keys of whichever
         * section preceded it. */
        if (strcmp(s, "[link]") == 0) {
            sec = SEC_LINK;
            continue;
        }
        if (strcmp(s, "[[capabilities]]") == 0) {
            sec = SEC_CAP;
            if (out->cap_count >= 8) {
                fclose(f);
                return -1;
            }
            cap_idx = (int)out->cap_count++;
            memset(&out->caps[cap_idx], 0, sizeof(out->caps[cap_idx]));
            continue;
        }
        if (strcmp(s, "[[regions]]") == 0) {
            sec = SEC_REGION;
            if (out->region_count >= WASMOS_APP_MAX_REGIONS) {
                fclose(f);
                return -1;
            }
            region_idx = (int)out->region_count++;
            out->regions[region_idx].kind = WASMOS_APP_REGION_IO;
            out->regions[region_idx].bar_index = 0;
            out->regions[region_idx].first = 0;
            out->regions[region_idx].last = 0;
            continue;
        }
        char* eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char* key = trim(s);
        char* val = trim(eq + 1);
        if (sec == SEC_DEPS) {
            uint8_t strength = 0;
            if (strcmp(key, "needs") == 0) {
                strength = DEP_NEEDS;
            } else if (strcmp(key, "wants") == 0) {
                strength = DEP_WANTS;
            } else if (strcmp(key, "provides") == 0) {
                strength = DEP_PROVIDES;
            } else {
                fprintf(stderr, "%s: unknown [deps] key '%s'\n", path, key);
                fclose(f);
                return -1;
            }
            if (parse_dep_list(path, val, strength, out) != 0) {
                fclose(f);
                return -1;
            }
            continue;
        }
        strip_quotes(val);

        if (sec == SEC_PACKAGE) {
            if (strcmp(key, "name") == 0) {
                snprintf(out->name, sizeof(out->name), "%s", val);
            } else if (strcmp(key, "entry") == 0) {
                snprintf(out->entry, sizeof(out->entry), "%s", val);
            } else if (strcmp(key, "kind") == 0) {
                snprintf(out->kind, sizeof(out->kind), "%s", val);
            } else if (strcmp(key, "subsystem") == 0) {
                snprintf(out->subsystem, sizeof(out->subsystem), "%s", val);
            } else if (strcmp(key, "native") == 0) {
                if (manifest_parse_bool(val, &out->native) != 0) {
                    fclose(f);
                    return -1;
                }
            } else if (strcmp(key, "storage_bootstrap") == 0) {
                if (manifest_parse_bool(val, &out->storage_bootstrap) != 0) {
                    fclose(f);
                    return -1;
                }
            } else if (strcmp(key, "wants_tty") == 0) {
                if (manifest_parse_bool(val, &out->wants_tty) != 0) {
                    fclose(f);
                    return -1;
                }
            }
        } else if (sec == SEC_REGION) {
            if (region_idx < 0) {
                continue;
            }
            if (strcmp(key, "kind") == 0) {
                if (strcmp(val, "io") == 0) {
                    out->regions[region_idx].kind = WASMOS_APP_REGION_IO;
                } else if (strcmp(val, "bar") == 0) {
                    out->regions[region_idx].kind = WASMOS_APP_REGION_BAR;
                } else {
                    fclose(f);
                    return -1;
                }
            } else if (strcmp(key, "index") == 0) {
                uint32_t v = 0;
                if (parse_u32_auto(val, &v) != 0 || v >= 6u) {
                    fclose(f);
                    return -1;
                }
                out->regions[region_idx].bar_index = (uint8_t)v;
            } else if (strcmp(key, "first") == 0) {
                uint32_t v = 0;
                if (parse_u32_auto(val, &v) != 0 || v > 0xFFFFu) {
                    fclose(f);
                    return -1;
                }
                out->regions[region_idx].first = (uint16_t)v;
            } else if (strcmp(key, "last") == 0) {
                uint32_t v = 0;
                if (parse_u32_auto(val, &v) != 0 || v > 0xFFFFu) {
                    fclose(f);
                    return -1;
                }
                out->regions[region_idx].last = (uint16_t)v;
            }
        } else if (sec == SEC_RESOURCES) {
            if (strcmp(key, "stack_pages") == 0) {
                if (parse_u32_auto(val, &out->stack_pages) != 0) {
                    fclose(f);
                    return -1;
                }
            } else if (strcmp(key, "heap_pages") == 0) {
                if (parse_u32_auto(val, &out->heap_pages) != 0) {
                    fclose(f);
                    return -1;
                }
            }
        } else if (sec == SEC_CAP && cap_idx >= 0) {
            if (strcmp(key, "name") == 0) {
                snprintf(out->caps[cap_idx].name, sizeof(out->caps[cap_idx].name), "%s", val);
            } else if (strcmp(key, "flags") == 0) {
                if (parse_u32_auto(val, &out->caps[cap_idx].flags) != 0) {
                    fclose(f);
                    return -1;
                }
            }
        }
    }
    fclose(f);
    if (out->name[0] == '\0' || out->entry[0] == '\0') {
        return -1;
    }
    if (out->subsystem[0] == '\0') {
        snprintf(out->subsystem, sizeof(out->subsystem), "%s", out->native ? "NATIVE" : "WASM");
    }
    if (!subsystem_tag_valid(out->subsystem)) {
        return -1;
    }
    return 0;
}

/* State of the --check-deps walk: one unit per manifest, and per unit the DFS
 * colour (0 unvisited, 1 on the current path, 2 finished). */
typedef struct {
    const char* path;
    linker_manifest_t manifest;
    uint8_t colour;
} dep_unit_t;

static int dep_provides(const linker_manifest_t* m, const manifest_dep_t* need) {
    for (uint32_t i = 0; i < m->dep_count; ++i) {
        const manifest_dep_t* d = &m->deps[i];
        if (d->strength == DEP_PROVIDES && d->kind == need->kind &&
            strcmp(d->name, need->name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Depth-first walk over NEEDS edges from units[u]. path[] holds the units on
 * the current path (depth entries). Returns 1 after printing the first cycle
 * found, 0 when none is reachable from u. */
static int dep_walk(dep_unit_t* units, size_t count, size_t u, size_t* path, size_t depth) {
    units[u].colour = 1;
    path[depth] = u;
    const linker_manifest_t* m = &units[u].manifest;
    for (uint32_t i = 0; i < m->dep_count; ++i) {
        const manifest_dep_t* need = &m->deps[i];
        if (need->strength != DEP_NEEDS) {
            continue;
        }
        for (size_t v = 0; v < count; ++v) {
            if (v == u || !dep_provides(&units[v].manifest, need)) {
                continue;
            }
            if (units[v].colour == 1) {
                fprintf(stderr, "dependency cycle over [deps] needs:");
                size_t start = 0;
                while (path[start] != v) {
                    start++;
                }
                for (size_t k = start; k <= depth; ++k) {
                    fprintf(stderr, " %s ->", units[path[k]].manifest.name);
                }
                fprintf(stderr,
                        " %s (closed by '%s%s' in %s)\n",
                        units[v].manifest.name,
                        need->kind == DEP_KIND_CLASS ? "class:" : "",
                        need->name,
                        units[u].path);
                return 1;
            }
            if (units[v].colour == 0 && dep_walk(units, count, v, path, depth + 1)) {
                return 1;
            }
        }
    }
    units[u].colour = 2;
    return 0;
}

/* --check-deps: parse every manifest with the packer's own rules and fail when
 * the NEEDS edges form a cycle. An edge runs from a unit to every OTHER unit
 * that provides the needed name or class, so a unit that both provides and
 * needs a class (a partition driver over disks) has no self-edge, and a class
 * need is conservatively an edge to each provider. WANTS never forms an edge:
 * a soft dependency may be circular. A NEEDS entry no manifest provides is
 * reported as a note and does not fail the check, since a provider may live
 * out of tree. Returns 0, or 1 on a parse failure or a cycle. */
static int check_deps(int count, char** paths) {
    if (count <= 0) {
        fprintf(stderr, "--check-deps needs at least one manifest\n");
        return 1;
    }
    dep_unit_t* units = (dep_unit_t*)calloc((size_t)count, sizeof(dep_unit_t));
    size_t* path = (size_t*)calloc((size_t)count + 1u, sizeof(size_t));
    if (!units || !path) {
        free(units);
        free(path);
        return 1;
    }
    int rc = 0;
    for (int i = 0; i < count; ++i) {
        units[i].path = paths[i];
        if (parse_linker_manifest(paths[i], &units[i].manifest) != 0) {
            fprintf(stderr, "failed to parse linker manifest: %s\n", paths[i]);
            rc = 1;
        }
    }
    for (int i = 0; rc == 0 && i < count; ++i) {
        const linker_manifest_t* m = &units[i].manifest;
        for (uint32_t d = 0; d < m->dep_count; ++d) {
            if (m->deps[d].strength != DEP_NEEDS) {
                continue;
            }
            int found = 0;
            for (int j = 0; j < count && !found; ++j) {
                found = j != i && dep_provides(&units[j].manifest, &m->deps[d]);
            }
            if (!found) {
                fprintf(stderr,
                        "note: %s needs '%s%s', which no checked manifest provides\n",
                        m->name,
                        m->deps[d].kind == DEP_KIND_CLASS ? "class:" : "",
                        m->deps[d].name);
            }
        }
    }
    for (int i = 0; rc == 0 && i < count; ++i) {
        if (units[i].colour == 0 && dep_walk(units, (size_t)count, (size_t)i, path, 0)) {
            rc = 1;
        }
    }
    free(path);
    free(units);
    return rc;
}

/* Pack one payload into a .wap package, or check dependencies. Three mutually
 * exclusive command lines:
 *
 *   --manifest <path> --in <in.wasm|elf> --out <out.wap> [--compiled <in.warpbin>]
 *       Preferred form. Everything except the payload comes from the linker
 *       manifest, and --compiled appends a pre-built WARP AOT binary.
 *
 *   --check-deps <manifest>...
 *       Writes nothing; fails on a cycle of [deps] needs edges (check_deps).
 *
 *   <in.wasm> <out.wap> <name> <entry> <stack_pages> <heap_pages> <flags>
 *   <cap_count> [<cap_name> <cap_flags>]...
 *       Positional form, kept for callers that have no manifest. This form
 *       emits no AOT binary, no region records and no dependencies.
 *
 * Writes the header and sections in the exact order src/kernel/wasmos_app.c
 * parses them, then the payload, then the AOT binary if present. Returns 0 on
 * success and 1 on any failure: bad arguments, an unknown capability name, a
 * non-zero capability flags value, an unreadable input, an empty --compiled
 * file, or a short write. A failed run may leave a truncated output file behind,
 * because errors after fopen do not remove it. */
int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--check-deps") == 0) {
        return check_deps(argc - 2, argv + 2);
    }
    if (argc >= 6 && strcmp(argv[1], "--manifest") == 0) {
        const char* manifest_path = argv[2];
        const char* in_path = NULL;
        const char* out_path = NULL;
        const char* compiled_path = NULL;
        for (int i = 3; i + 1 < argc; i += 2) {
            if (strcmp(argv[i], "--in") == 0) {
                in_path = argv[i + 1];
            } else if (strcmp(argv[i], "--out") == 0) {
                out_path = argv[i + 1];
            } else if (strcmp(argv[i], "--compiled") == 0) {
                compiled_path = argv[i + 1];
            } else {
                fprintf(stderr, "unknown flag '%s'\n", argv[i]);
                return 1;
            }
        }
        if (!in_path || !out_path) {
            fprintf(stderr,
                    "usage: %s --manifest <path> --in <in.wasm|elf> --out <out.wap> [--compiled "
                    "<in.warpbin>]\n",
                    argv[0]);
            return 1;
        }
        linker_manifest_t lm;
        if (parse_linker_manifest(manifest_path, &lm) != 0) {
            fprintf(stderr, "failed to parse linker manifest: %s\n", manifest_path);
            return 1;
        }
        uint32_t flags = 0;
        if (strcmp(lm.kind, "driver") == 0)
            flags |= FLAG_DRIVER;
        else if (strcmp(lm.kind, "service") == 0)
            flags |= FLAG_SERVICE;
        else
            flags |= FLAG_APP;
        if (lm.native)
            flags |= (1u << 4);
        if (lm.storage_bootstrap)
            flags |= (1u << 5);
        if (lm.wants_tty)
            flags |= (1u << 6);

        uint32_t cap_count = lm.cap_count;
        const char* cap_names[8];
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

        FILE* in = fopen(in_path, "rb");
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
        uint8_t* wasm = (uint8_t*)malloc((size_t)in_size);
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

        /* Optional: load pre-compiled WARP AOT binary. */
        uint8_t* compiled_data = NULL;
        size_t compiled_data_size = 0;
        if (compiled_path) {
            FILE* cf = fopen(compiled_path, "rb");
            if (!cf) {
                perror("open compiled");
                free(wasm);
                return 1;
            }
            if (fseek(cf, 0, SEEK_END) != 0) {
                perror("seek compiled");
                fclose(cf);
                free(wasm);
                return 1;
            }
            long csz = ftell(cf);
            if (csz <= 0) {
                fprintf(stderr, "compiled file is empty\n");
                fclose(cf);
                free(wasm);
                return 1;
            }
            fseek(cf, 0, SEEK_SET);
            compiled_data = (uint8_t*)malloc((size_t)csz);
            if (!compiled_data) {
                fclose(cf);
                free(wasm);
                return 1;
            }
            if (fread(compiled_data, 1, (size_t)csz, cf) != (size_t)csz) {
                perror("read compiled");
                fclose(cf);
                free(compiled_data);
                free(wasm);
                return 1;
            }
            fclose(cf);
            compiled_data_size = (size_t)csz;
        }

        FILE* outf = fopen(out_path, "wb");
        if (!outf) {
            perror("open output");
            free(compiled_data);
            free(wasm);
            return 1;
        }

        /* Zero-initialised so a header field added later cannot reach the package
         * as stack garbage on a path that forgets to assign it. */
        wasmos_app_header_t hdr = {0};
        memcpy(hdr.magic, MAGIC, 8);
        hdr.version = VERSION;
        hdr.header_size = sizeof(hdr);
        hdr.flags = flags;
        hdr.name_len = (uint32_t)strlen(lm.name);
        hdr.entry_len = (uint32_t)strlen(lm.entry);
        hdr.wasm_size = (uint32_t)in_size;
        hdr.dep_count = lm.dep_count;
        hdr.cap_count = cap_count;
        hdr.mem_hint_count = 2;
        hdr.compiled_size = (uint32_t)compiled_data_size;
        subsystem_tag_copy(hdr.subsystem_tag, lm.subsystem);
        hdr.region_count = lm.region_count;

        wasmos_mem_hint_t stack_hint = {MEM_HINT_STACK, lm.stack_pages, 0};
        wasmos_mem_hint_t heap_hint = {MEM_HINT_HEAP, lm.heap_pages, 0};

        int ok = 1;
        ok &= fwrite(&hdr, sizeof(hdr), 1, outf) == 1;
        ok &= fwrite(lm.name, 1, hdr.name_len, outf) == hdr.name_len;
        ok &= fwrite(lm.entry, 1, hdr.entry_len, outf) == hdr.entry_len;
        for (uint32_t i = 0; i < lm.dep_count; ++i) {
            wasmos_dep_record_t rec;
            rec.strength = lm.deps[i].strength;
            rec.kind = lm.deps[i].kind;
            rec.name_len = (uint16_t)strlen(lm.deps[i].name);
            ok &= fwrite(&rec, sizeof(rec), 1, outf) == 1;
            ok &= fwrite(lm.deps[i].name, 1, rec.name_len, outf) == rec.name_len;
        }
        for (uint32_t i = 0; i < cap_count; ++i) {
            ok &= fwrite(&caps[i], sizeof(caps[i]), 1, outf) == 1;
            ok &= fwrite(cap_names[i], 1, caps[i].name_len, outf) == caps[i].name_len;
        }
        for (uint32_t i = 0; i < lm.region_count; ++i) {
            wasmos_region_entry_t region_entry;
            region_entry.kind = lm.regions[i].kind;
            region_entry.bar_index = lm.regions[i].bar_index;
            region_entry.first = lm.regions[i].first;
            region_entry.last = lm.regions[i].last;
            ok &= fwrite(&region_entry, sizeof(region_entry), 1, outf) == 1;
        }
        ok &= fwrite(&stack_hint, sizeof(stack_hint), 1, outf) == 1;
        ok &= fwrite(&heap_hint, sizeof(heap_hint), 1, outf) == 1;
        ok &= fwrite(wasm, 1, (size_t)in_size, outf) == (size_t)in_size;
        if (compiled_data_size > 0) {
            ok &= fwrite(compiled_data, 1, compiled_data_size, outf) == compiled_data_size;
        }

        free(compiled_data);
        free(wasm);
        fclose(outf);
        return ok ? 0 : 1;
    }
    if (argc < 9) {
        fprintf(stderr,
                "usage: %s <in.wasm> <out.wap> <name> <entry> <stack_pages> <heap_pages> <flags> "
                "<cap_count> [<cap_name> <cap_flags>]...\n"
                "       %s --manifest <path> --in <in.wasm|elf> --out <out.wap> [--compiled "
                "<in.warpbin>]\n"
                "       %s --check-deps <manifest>...\n",
                argv[0],
                argv[0],
                argv[0]);
        return 1;
    }

    const char* in_path = argv[1];
    const char* out_path = argv[2];
    const char* name = argv[3];
    const char* entry = argv[4];
    uint32_t stack_pages = 0;
    uint32_t heap_pages = 0;
    uint32_t flags = 0;
    uint32_t cap_count = 0;
    const uint32_t cap_max = 8;
    const char* cap_names[8];
    wasmos_cap_request_t caps[8];
    if (parse_u32(argv[5], &stack_pages) != 0 || parse_u32(argv[6], &heap_pages) != 0) {
        fprintf(stderr, "invalid stack/heap page value\n");
        return 1;
    }
    if (parse_u32(argv[7], &flags) != 0) {
        fprintf(stderr, "invalid flags value\n");
        return 1;
    }
    if (parse_u32(argv[8], &cap_count) != 0) {
        fprintf(stderr, "invalid cap_count value\n");
        return 1;
    }
    int min_tail = (int)(9u + (cap_count * 2u));
    if (argc < min_tail) {
        fprintf(stderr, "invalid capability argument layout\n");
        return 1;
    }
    if (cap_count > cap_max) {
        fprintf(stderr, "cap_count exceeds max supported entries (%u)\n", cap_max);
        return 1;
    }
    for (uint32_t i = 0; i < cap_count; ++i) {
        const char* cap_name = argv[9 + (i * 2u)];
        uint32_t cap_flags = 0;
        if (!cap_name || cap_name[0] == '\0' || (cap_name[0] == '-' && cap_name[1] == '\0') ||
            parse_u32(argv[10 + (i * 2u)], &cap_flags) != 0) {
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
    /* The positional form takes no trailing options: everything past the
     * capability pairs would have to be a flag, and none remain. */
    if (argc > min_tail) {
        fprintf(stderr, "unknown trailing argument '%s'\n", argv[min_tail]);
        return 1;
    }

    FILE* in = fopen(in_path, "rb");
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

    uint8_t* wasm = (uint8_t*)malloc((size_t)in_size);
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

    FILE* out = fopen(out_path, "wb");
    if (!out) {
        perror("open output");
        free(wasm);
        return 1;
    }

    /* Zero-initialised: see the manifest path's note. */
    wasmos_app_header_t hdr = {0};
    memcpy(hdr.magic, MAGIC, 8);
    hdr.version = VERSION;
    hdr.header_size = sizeof(hdr);
    hdr.flags = flags;
    hdr.name_len = (uint32_t)strlen(name);
    hdr.entry_len = (uint32_t)strlen(entry);
    hdr.wasm_size = (uint32_t)in_size;
    hdr.dep_count = 0; /* dependencies need the manifest's [deps] table */
    hdr.cap_count = cap_count;
    hdr.mem_hint_count = 2;
    hdr.compiled_size = 0; /* positional path emits no AOT binary; use --manifest --compiled */
    subsystem_tag_copy(hdr.subsystem_tag, (flags & (1u << 4)) != 0 ? "NATIVE" : "WASM");
    /* No regions without a manifest: declaring a register window needs the
     * [[regions]] table, so this path emits none.  The count must still be
     * written, because the parser consumes exactly this many region entries
     * before the memory hints and would otherwise read the following section at
     * the wrong offset. */
    hdr.region_count = 0;

    wasmos_mem_hint_t stack_hint = {MEM_HINT_STACK, stack_pages, 0};
    wasmos_mem_hint_t heap_hint = {MEM_HINT_HEAP, heap_pages, 0};

    int ok = 1;
    ok &= fwrite(&hdr, sizeof(hdr), 1, out) == 1;
    ok &= fwrite(name, 1, hdr.name_len, out) == hdr.name_len;
    ok &= fwrite(entry, 1, hdr.entry_len, out) == hdr.entry_len;
    for (uint32_t i = 0; i < cap_count; ++i) {
        ok &= fwrite(&caps[i], sizeof(caps[i]), 1, out) == 1;
        ok &= fwrite(cap_names[i], 1, caps[i].name_len, out) == caps[i].name_len;
    }
    ok &= fwrite(&stack_hint, sizeof(stack_hint), 1, out) == 1;
    ok &= fwrite(&heap_hint, sizeof(heap_hint), 1, out) == 1;
    ok &= fwrite(wasm, 1, (size_t)in_size, out) == (size_t)in_size;

    free(wasm);
    fclose(out);
    return ok ? 0 : 1;
}
