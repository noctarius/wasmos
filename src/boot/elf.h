/* elf.h - ELF64 header and program-header structs used by the bootloader.
 * Only PT_LOAD segments are processed; section headers are ignored at load time. */
#ifndef WASMOS_ELF_H
#define WASMOS_ELF_H

#include <stdint.h>

#define EI_NIDENT 16
#define PT_LOAD 1 /* loadable segment type */

/* ELF64 executable header. e_entry addresses _start, which the kernel's linker
 * script places in the identity-mapped bootstrap at KERNEL_LOAD_BASE, so the
 * bootloader can call it directly before paging is installed. */
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

/* ELF64 program header. p_memsz >= p_filesz; the gap must be zero-filled (BSS). */
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
