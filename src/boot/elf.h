/* elf.h - ELF64 header and program-header structs used by the bootloader.
 * Only PT_LOAD segments are processed; section headers are ignored at load time. */
#ifndef WASMOS_ELF_H
#define WASMOS_ELF_H

#include <stdint.h>

/* Size of Elf64_Ehdr.e_ident, fixed by the ELF specification. */
#define EI_NIDENT 16
#define PT_LOAD 1 /* loadable segment type */

/* ELF64 executable header. e_entry addresses _start, which the kernel's linker
 * script places in the identity-mapped bootstrap at KERNEL_LOAD_BASE, so the
 * bootloader can call it directly before paging is installed.
 *
 * Fields the loader consumes: e_ident (only the four magic bytes are checked,
 * so class/endianness/ABI go unvalidated), e_entry, e_phoff, e_phnum and
 * e_entry's callee ABI. e_phentsize is assumed to equal sizeof(Elf64_Phdr)
 * rather than being read, and e_shoff/e_shnum/e_shentsize/e_shstrndx are ignored
 * outright: the kernel image carries no relocations to apply at load time. */
typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

/* ELF64 program header. p_memsz >= p_filesz; the gap must be zero-filled (BSS).
 *
 * Only entries with p_type == PT_LOAD are processed; p_flags (segment R/W/X
 * permissions) is not consulted, because the bootstrap page tables entry.S
 * builds map everything writable and executable and the kernel installs the real
 * permissions later. p_align is likewise ignored: the loader rounds p_paddr down
 * to a 4 KiB page itself. */
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset; /* byte offset in the ELF file */
    uint64_t p_vaddr;  /* virtual address the kernel links against */
    uint64_t p_paddr;  /* load address; the bootloader copies here, not to p_vaddr */
    uint64_t p_filesz; /* bytes present in the file (may be 0 for BSS-only) */
    uint64_t p_memsz;  /* bytes to occupy in memory */
    uint64_t p_align;
} Elf64_Phdr;

#endif
