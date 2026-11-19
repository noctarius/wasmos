#include "supervisor_unit.h"

#include <string.h>

#include "../../../abi/generated/c/wasmos_constants.h"
#include "../../../abi/generated/c/wasmos_status.h"

_Static_assert(sizeof(((wasmos_supervisor_unit_record_t*)0)->name) == SV_UNIT_NAME_MAX,
               "a unit name is copied whole into its list record");

static int sv_due(uint32_t deadline, uint32_t now) {
    return (int32_t)(now - deadline) >= 0;
}

static int sv_is_space(char c) {
    return c == ' ' || c == '\t';
}

static const char* sv_skip_space(const char* s) {
    while (sv_is_space(*s)) {
        s++;
    }
    return s;
}

static uint32_t sv_token_len(const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0' && !sv_is_space(s[n])) {
        n++;
    }
    return n;
}

static int sv_token_is(const char* s, uint32_t len, const char* word) {
    uint32_t wlen = (uint32_t)strlen(word);
    return len == wlen && memcmp(s, word, len) == 0;
}

/* Parses an unsigned decimal of 1..3 digits filling exactly `len` bytes. */
static int sv_parse_small(const char* s, uint32_t len, uint32_t* out) {
    uint32_t v = 0;
    if (len == 0 || len > 3) {
        return -1;
    }
    for (uint32_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            return -1;
        }
        v = v * 10u + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int sv_parse_option(sv_unit_t* unit, const char* tok, uint32_t len) {
    const char* eq = 0;
    for (uint32_t i = 0; i < len; ++i) {
        if (tok[i] == '=') {
            eq = &tok[i];
            break;
        }
    }
    if (!eq) {
        return -1;
    }
    uint32_t key_len = (uint32_t)(eq - tok);
    const char* val = eq + 1;
    uint32_t val_len = len - key_len - 1u;

    if (sv_token_is(tok, key_len, "restart")) {
        if (sv_token_is(val, val_len, "never")) {
            unit->restart = SV_RESTART_NEVER;
        } else if (sv_token_is(val, val_len, "on-failure")) {
            unit->restart = SV_RESTART_ON_FAILURE;
        } else if (sv_token_is(val, val_len, "always")) {
            unit->restart = SV_RESTART_ALWAYS;
        } else {
            return -1;
        }
        return 0;
    }
    if (sv_token_is(tok, key_len, "class")) {
        if (sv_token_is(val, val_len, "optional")) {
            unit->cls = SV_CLASS_OPTIONAL;
        } else if (sv_token_is(val, val_len, "required")) {
            unit->cls = SV_CLASS_REQUIRED;
        } else if (sv_token_is(val, val_len, "critical")) {
            unit->cls = SV_CLASS_CRITICAL;
        } else {
            return -1;
        }
        return 0;
    }
    if (sv_token_is(tok, key_len, "tty")) {
        uint32_t slot = 0;
        /* Slot 0 is no controlling slot -- the child's console output goes to
         * the kernel log -- so it is not a pin; the PM hands shells 1 and up.
         * The descriptor carries slot + 1. */
        if (sv_parse_small(val, val_len, &slot) != 0 || slot == 0u || slot > 254u) {
            return -1;
        }
        unit->tty = slot + 1u;
        return 0;
    }
    if (sv_token_is(tok, key_len, "probe")) {
        uint32_t seconds = 0;
        if (sv_parse_small(val, val_len, &seconds) != 0 || seconds == 0u) {
            return -1;
        }
        unit->probe_s = seconds;
        return 0;
    }
    return -1;
}

int32_t sv_unit_set_caps(sv_unit_t* unit, const uint8_t* caps, uint32_t len) {
    wasmos_spawn_caps_v2_t hdr;
    if (!unit) {
        return WASMOS_ERR_SUPERVISOR_BAD_DESC;
    }
    if (len == 0u) {
        unit->caps_len = 0;
        return 0;
    }
    if (!caps || len < (uint32_t)sizeof(hdr) || len > SUPERVISOR_CAPS_MAX) {
        return WASMOS_ERR_SUPERVISOR_BAD_DESC;
    }
    memcpy(&hdr, caps, sizeof(hdr));
    if ((hdr.cap_flags & DEVMGR_CAP_DMA) != 0u) {
        return WASMOS_ERR_SUPERVISOR_DMA_CAPS;
    }
    memcpy(unit->caps, caps, len);
    unit->caps_len = len;
    return 0;
}

int32_t sv_spec_parse(const char* spec, const sv_timing_t* timing, uint32_t now, sv_unit_t* out) {
    sv_unit_t unit;
    const char* p;
    uint32_t len;

    if (!spec || !timing || !out) {
        return WASMOS_ERR_SUPERVISOR_BAD_SPEC;
    }
    memset(&unit, 0, sizeof(unit));
    unit.restart = SV_RESTART_ON_FAILURE;
    unit.cls = SV_CLASS_OPTIONAL;

    p = sv_skip_space(spec);
    len = sv_token_len(p);
    if (len == 0 || len >= SV_UNIT_NAME_MAX) {
        return WASMOS_ERR_SUPERVISOR_BAD_SPEC;
    }
    memcpy(unit.name, p, len);
    p = sv_skip_space(p + len);

    len = sv_token_len(p);
    if (len == 0 || len >= SV_PATH_MAX || p[0] != '/') {
        return WASMOS_ERR_SUPERVISOR_BAD_SPEC;
    }
    memcpy(unit.path, p, len);
    p = sv_skip_space(p + len);

    while (*p != '\0') {
        len = sv_token_len(p);
        if (sv_token_is(p, len, "--")) {
            const char* args = sv_skip_space(p + len);
            uint32_t args_len = (uint32_t)strlen(args);
            while (args_len > 0 && sv_is_space(args[args_len - 1u])) {
                args_len--;
            }
            if (args_len >= SV_ARGS_MAX) {
                return WASMOS_ERR_SUPERVISOR_BAD_SPEC;
            }
            memcpy(unit.args, args, args_len);
            break;
        }
        if (sv_parse_option(&unit, p, len) != 0) {
            return WASMOS_ERR_SUPERVISOR_BAD_SPEC;
        }
        p = sv_skip_space(p + len);
    }

    unit.state = SV_UNIT_SPAWN_DUE;
    unit.deadline = now;
    unit.backoff_ms = timing->backoff_min_ms;
    unit.probe_ep = SV_PROBE_NONE;
    *out = unit;
    return 0;
}

/* The unit ended (or never started). `failure` says whether the end counts
 * against it; the restart policy and the intensity window decide the rest.
 * Every end that would restart the unit is recorded in the window, clean ends
 * under restart=always included, so a unit that exits at once in a loop gives
 * up like one that crashes. */
static sv_verdict_t sv_unit_decide(sv_unit_t* unit, const sv_timing_t* timing, int failure,
                                   uint32_t now) {
    uint32_t in_window = 0;

    unit->pid = 0;
    unit->probe_ep = SV_PROBE_NONE;
    unit->probe_pending = 0;
    if (unit->cls == SV_CLASS_CRITICAL) {
        unit->state = SV_UNIT_FAILED;
        return SV_VERDICT_CRITICAL;
    }
    if (unit->restart == SV_RESTART_NEVER || (unit->restart == SV_RESTART_ON_FAILURE && !failure)) {
        unit->state = failure ? SV_UNIT_FAILED : SV_UNIT_STOPPED;
        return failure ? SV_VERDICT_FAILED : SV_VERDICT_STOPPED;
    }

    unit->ends_at[unit->ends_head] = now;
    unit->ends_head = (unit->ends_head + 1u) % SV_INTENSITY_SLOTS;
    if (unit->ends_recorded < SV_INTENSITY_SLOTS) {
        unit->ends_recorded++;
    }
    for (uint32_t i = 0; i < unit->ends_recorded; ++i) {
        if (now - unit->ends_at[i] < timing->intensity_window_ms) {
            in_window++;
        }
    }
    if (in_window >= timing->intensity_max) {
        unit->state = SV_UNIT_FAILED;
        return SV_VERDICT_FAILED;
    }

    unit->state = SV_UNIT_SPAWN_DUE;
    unit->deadline = now + unit->backoff_ms;
    unit->backoff_ms = unit->backoff_ms > timing->backoff_max_ms / 2u ? timing->backoff_max_ms
                                                                      : unit->backoff_ms * 2u;
    unit->restart_count++;
    return SV_VERDICT_RESTART;
}

void sv_unit_spawn_sent(sv_unit_t* unit) {
    if (unit && unit->state == SV_UNIT_SPAWN_DUE) {
        unit->state = SV_UNIT_SPAWNING;
    }
}

void sv_unit_spawned(sv_unit_t* unit, const sv_timing_t* timing, uint32_t pid, uint32_t now) {
    if (!unit || !timing || unit->state != SV_UNIT_SPAWNING) {
        return;
    }
    unit->pid = pid;
    unit->state = SV_UNIT_STARTING;
    unit->deadline = now + timing->ready_timeout_ms;
    unit->probe_ep = SV_PROBE_NONE;
    unit->probe_pending = 0;
    unit->probes_answered = 0;
    unit->probe_misses = 0;
}

sv_verdict_t sv_unit_spawn_failed(sv_unit_t* unit, const sv_timing_t* timing, int32_t code,
                                  uint32_t now) {
    if (!unit || !timing || unit->state != SV_UNIT_SPAWNING) {
        return SV_VERDICT_NONE;
    }
    unit->last_status = code;
    unit->last_reason = PROC_EXIT_REASON_START_FAILED;
    return sv_unit_decide(unit, timing, 1, now);
}

void sv_unit_ready(sv_unit_t* unit, const sv_timing_t* timing, uint32_t probe_ep, uint32_t now) {
    if (!unit || !timing || unit->state != SV_UNIT_STARTING) {
        return;
    }
    unit->state = SV_UNIT_RUNNING;
    unit->deadline = now + timing->stable_ms;
    unit->probe_ep = probe_ep;
    unit->probe_due = now;
}

/* Probed and not yet answered once: READY alone does not show the loop runs. */
static int sv_awaits_first_answer(const sv_unit_t* unit) {
    return unit->probe_ep != SV_PROBE_NONE && unit->probes_answered == 0;
}

static int sv_is_probed(const sv_unit_t* unit) {
    return unit->probe_ep != SV_PROBE_NONE &&
           (unit->state == SV_UNIT_RUNNING || unit->state == SV_UNIT_STABLE);
}

uint32_t sv_unit_probe_interval(const sv_unit_t* unit, const sv_timing_t* timing) {
    if (!unit || !timing) {
        return 0;
    }
    return unit->probe_s != 0u ? unit->probe_s * 1000u * timing->scale : timing->probe_interval_ms;
}

sv_verdict_t sv_unit_probe_tick(sv_unit_t* unit, const sv_timing_t* timing, uint32_t now) {
    if (!unit || !timing || !sv_is_probed(unit) || !sv_due(unit->probe_due, now)) {
        return SV_VERDICT_NONE;
    }
    if (unit->probe_pending != 0u) {
        unit->probe_pending = 0;
        unit->probe_misses++;
        if (unit->probe_misses >= timing->probe_misses_max) {
            unit->state = SV_UNIT_STOPPING;
            return SV_VERDICT_HUNG;
        }
    }
    return SV_VERDICT_PROBE;
}

void sv_unit_probe_sent(sv_unit_t* unit, const sv_timing_t* timing, uint32_t request_id,
                        uint32_t now) {
    if (!unit || !timing || !sv_is_probed(unit)) {
        return;
    }
    unit->probe_pending = request_id;
    unit->probe_due = now + sv_unit_probe_interval(unit, timing);
}

int sv_unit_probe_answered(sv_unit_t* unit, const sv_timing_t* timing, uint32_t request_id,
                           uint32_t now) {
    if (!unit || !timing || !sv_is_probed(unit) || request_id == 0u ||
        unit->probe_pending != request_id) {
        return 0;
    }
    unit->probe_pending = 0;
    unit->probe_misses = 0;
    unit->probes_answered++;
    if (unit->state == SV_UNIT_RUNNING && sv_due(unit->deadline, now)) {
        unit->state = SV_UNIT_STABLE;
        unit->backoff_ms = timing->backoff_min_ms;
    }
    return 1;
}

sv_verdict_t sv_unit_exited(sv_unit_t* unit, const sv_timing_t* timing, int32_t status,
                            uint32_t reason, uint32_t now) {
    int failure;
    if (!unit || !timing) {
        return SV_VERDICT_NONE;
    }
    if (unit->state != SV_UNIT_STARTING && unit->state != SV_UNIT_RUNNING &&
        unit->state != SV_UNIT_STABLE && unit->state != SV_UNIT_STOPPING) {
        return SV_VERDICT_NONE;
    }
    unit->last_status = status;
    unit->last_reason = reason;
    /* A unit the supervisor killed for missing READY failed, whatever its
     * exit says. */
    failure = unit->state == SV_UNIT_STOPPING || sv_exit_is_failure(status, reason);
    return sv_unit_decide(unit, timing, failure, now);
}

sv_verdict_t sv_unit_tick(sv_unit_t* unit, const sv_timing_t* timing, uint32_t now) {
    if (!unit || !timing || !sv_due(unit->deadline, now)) {
        return SV_VERDICT_NONE;
    }
    switch (unit->state) {
    case SV_UNIT_SPAWN_DUE:
        return SV_VERDICT_SPAWN;
    case SV_UNIT_STARTING:
        unit->state = SV_UNIT_STOPPING;
        return SV_VERDICT_KILL;
    case SV_UNIT_RUNNING:
        if (sv_awaits_first_answer(unit)) {
            return SV_VERDICT_NONE;
        }
        unit->state = SV_UNIT_STABLE;
        unit->backoff_ms = timing->backoff_min_ms;
        return SV_VERDICT_NONE;
    default:
        return SV_VERDICT_NONE;
    }
}

int sv_unit_deadline(const sv_unit_t* unit, uint32_t* out) {
    uint32_t earliest = 0;
    int have = 0;
    if (!unit) {
        return 0;
    }
    /* A running unit's stable deadline waits for its first answer instead,
     * which promotes it (sv_unit_probe_answered). */
    if (unit->state == SV_UNIT_SPAWN_DUE || unit->state == SV_UNIT_STARTING ||
        (unit->state == SV_UNIT_RUNNING && !sv_awaits_first_answer(unit))) {
        earliest = unit->deadline;
        have = 1;
    }
    if (sv_is_probed(unit) && (!have || (int32_t)(unit->probe_due - earliest) < 0)) {
        earliest = unit->probe_due;
        have = 1;
    }
    if (have && out) {
        *out = earliest;
    }
    return have;
}

uint32_t sv_units_list(const sv_unit_t* units, uint32_t n, const sv_timing_t* timing,
                       wasmos_supervisor_unit_record_t* out, uint32_t capacity, uint32_t* total) {
    uint32_t held = 0;
    uint32_t written = 0;
    for (uint32_t i = 0; units && i < n; ++i) {
        const sv_unit_t* u = &units[i];
        wasmos_supervisor_unit_record_t* r;
        if (u->state == SV_UNIT_FREE) {
            continue;
        }
        held++;
        if (!out || written >= capacity) {
            continue;
        }
        r = &out[written++];
        memset(r, 0, sizeof(*r));
        memcpy(r->name, u->name, sizeof(u->name));
        r->state = (uint32_t)u->state;
        r->restart = (uint32_t)u->restart;
        r->cls = (uint32_t)u->cls;
        r->tty = u->tty;
        r->pid = u->pid;
        r->restart_count = u->restart_count;
        r->last_reason = u->last_reason;
        r->last_status = u->last_status;
        r->probe_interval_ms = sv_is_probed(u) ? sv_unit_probe_interval(u, timing) : 0u;
        r->probes_answered = u->probes_answered;
        r->probe_misses = u->probe_misses;
    }
    if (total) {
        *total = held;
    }
    return written;
}

int sv_unit_is_up(const sv_unit_t* unit) {
    return unit && (unit->state == SV_UNIT_RUNNING || unit->state == SV_UNIT_STABLE) &&
           !sv_awaits_first_answer(unit);
}

sv_boot_t sv_boot_check(const sv_unit_t* units, uint32_t n, uint32_t* count, uint32_t* index) {
    uint32_t up = 0;
    int pending = 0;
    for (uint32_t i = 0; units && i < n; ++i) {
        const sv_unit_t* u = &units[i];
        if (u->state == SV_UNIT_FREE || !u->boot || u->cls == SV_CLASS_OPTIONAL) {
            continue;
        }
        if (u->state == SV_UNIT_FAILED || u->state == SV_UNIT_STOPPED) {
            if (index) {
                *index = i;
            }
            return SV_BOOT_FAILED;
        }
        if (sv_unit_is_up(u)) {
            up++;
        } else {
            pending = 1;
        }
    }
    if (pending) {
        return SV_BOOT_PENDING;
    }
    if (count) {
        *count = up;
    }
    return SV_BOOT_COMPLETE;
}

int sv_exit_is_failure(int32_t status, uint32_t reason) {
    return !(reason == PROC_EXIT_REASON_EXITED && status == 0);
}

const char* sv_state_name(sv_state_t state) {
    switch (state) {
    case SV_UNIT_SPAWN_DUE:
        return "spawn-due";
    case SV_UNIT_SPAWNING:
        return "spawning";
    case SV_UNIT_STARTING:
        return "starting";
    case SV_UNIT_RUNNING:
        return "running";
    case SV_UNIT_STABLE:
        return "stable";
    case SV_UNIT_STOPPING:
        return "stopping";
    case SV_UNIT_STOPPED:
        return "stopped";
    case SV_UNIT_FAILED:
        return "failed";
    default:
        return "free";
    }
}

const char* sv_reason_name(uint32_t reason) {
    switch (reason) {
    case PROC_EXIT_REASON_EXITED:
        return "exited";
    case PROC_EXIT_REASON_TRAPPED:
        return "trapped";
    case PROC_EXIT_REASON_FAULTED:
        return "faulted";
    case PROC_EXIT_REASON_KILLED:
        return "killed";
    case PROC_EXIT_REASON_START_FAILED:
        return "start failed";
    default:
        return "unknown";
    }
}
