/* device_manager_rules.h - parser declarations for device-manager rule files.
 *
 * Rule-file format (a udev-flavoured subset; see
 * scripts/system/devmgr/rules/default.rules for a live example):
 *
 *   - Line oriented, one rule per line, no continuations.  A line whose first
 *     non-blank character is '#' is a comment; an unquoted '#' anywhere else
 *     truncates the rest of the line.  Trailing whitespace is trimmed.
 *   - A rule is a comma-separated list of `KEY OP "value"` fields.  Whitespace
 *     around a field is ignored, and a comma inside double quotes does not
 *     split.  The operator is matched literally: `==` for a match field, `+=`
 *     for RUN, `=` for ENV{MOUNT}.  The value must be double quoted.
 *   - `SUBSYSTEM=="..."` selects which of the four kinds a line belongs to
 *     ("boot", "block", "pci", "acpi"), and `RUN+="<path>"` names the driver
 *     `.wap` to spawn.  Both are mandatory; a line missing either — or carrying
 *     a SUBSYSTEM that does not match the loader being run — is skipped
 *     silently, which is exactly how one file feeds all four loaders.
 *   - `ATTR{...}=="..."` fields narrow the match.  For pci/acpi they are
 *     hexadecimal (with or without a `0x` prefix); an omitted ATTR leaves the
 *     field at MATCH_ANY_U8/MATCH_ANY_U16, i.e. a wildcard.  `ATTR{unit}` in a
 *     block rule is decimal, or the literal "any".  A value that does not parse
 *     rejects the whole line.
 *   - `ENV{MOUNT}="..."` applies to block rules only and defaults to "/".
 *   - Unrecognised fields are ignored rather than rejected.
 *
 * Limits, all of which drop input silently: a line longer than 255 characters is
 * truncated, a RUN path of 96 bytes or more makes the line unparseable, and
 * rules past the per-kind cap (ALWAYS_SPAWN_RULE_CAP and friends) are not
 * stored.  The file itself is read into DEVMGR_RULE_TEXT_CAP bytes.
 *
 * `state` is borrowed for the call in every function below; `text` is a
 * NUL-terminated buffer the caller owns and is not modified.  Each loader first
 * clears its own rule table, so loading a second file REPLACES that kind rather
 * than appending to it — the boot-FAT rules therefore supersede the initfs
 * bootstrap rules for every kind the boot file mentions, and clear the kinds it
 * does not.  A NULL state or text is a no-op. */
#ifndef WASMOS_DEVICE_MANAGER_RULES_H
#define WASMOS_DEVICE_MANAGER_RULES_H

#include <stdint.h>
#include "device_manager_types.h"

/* Count non-blank, non-comment lines in text; used to pre-check rule budget.
 * This is a raw line count: it does not parse, so it also counts lines that the
 * loaders will later reject, and it counts a line of every SUBSYSTEM kind.
 * Returns 0 for NULL. */
uint16_t dm_rules_count_active(const char* text);
/* Parse always_spawn rules (boot-time unconditional driver spawns) from text.
 * Accepts SUBSYSTEM=="boot" lines; fills state->always_spawn_rules[] with
 * active=1, queued=1, spawned=0 and sets state->always_spawn_rule_count. */
void dm_rules_load_always_spawn(device_manager_state_t* state, const char* text);
/* Parse block_fs rules (block-device subsystem with mount points) from text.
 * Accepts SUBSYSTEM=="block" lines, honouring ATTR{unit} and ENV{MOUNT}.  Rules
 * are stored queued=0 — they are queued later, when a matching block device is
 * reported — and state->active_rule_spawn_index is reset to -1. */
void dm_rules_load_block_fs(device_manager_state_t* state, const char* text);
/* Parse pci_match rules (PCI class/vendor/device driver binding) from text.
 * Accepts SUBSYSTEM=="pci" lines with any of ATTR{bus,slot,function,class,
 * subclass,prog_if,vendor,device}.  Each rule's spawned_device_mask is cleared,
 * so reloading rules lets an already-matched device be spawned again. */
void dm_rules_load_pci_match(device_manager_state_t* state, const char* text);
/* Parse acpi_match rules (ACPI/ISA class driver binding) from text.  Accepts
 * SUBSYSTEM=="acpi" lines with ATTR{class} and ATTR{subclass}; like the PCI
 * loader it clears each rule's spawned_device_mask. */
void dm_rules_load_acpi_match(device_manager_state_t* state, const char* text);

#endif
