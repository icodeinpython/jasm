#define _XOPEN_SOURCE 500

#include "jasm.h"


#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>


static size_t find_name_offset(const char *sh_names, const char *name) {
    const char *p = strstr(sh_names, name);
    return p ? (size_t)(p - sh_names) : 0;
}

void init_reloc_table(RelocTable *tbl) {
    tbl->entries = NULL;
    tbl->count = 0;
    tbl->capacity = 0;
}

static void grow_reloc_table(RelocTable *tbl) {
    size_t new_cap = tbl->capacity ? tbl->capacity * 2 : 8;
    Reloc* new_entries = realloc(tbl->entries, new_cap * sizeof(Reloc));
    if (!new_entries) { perror("realloc"); exit(1); }
    tbl->entries = new_entries;
    tbl->capacity = new_cap;
}

void emit_reloc(RelocTable *tbl, Label* label, uint32_t offset, Section section) {
    if (tbl->count >= tbl->capacity) {
        grow_reloc_table(tbl);
    }

    printf("emit reloc: %s, offset: %#x, section: %d\n", label->name, offset, section);

    Reloc *r = &tbl->entries[tbl->count++];
    r->label = label;
    r->offset = offset;
    r->section = section;
}

static size_t find_sym_index(const LabelTable *labels, const char *name) {
    size_t i;
    for (i = 0; i < labels->count; i++) {
        if (strcmp(name, labels->entries[i].name) == 0)
            return i + 1; /* +1 since sym[0] = undefined */
    }
    return 0;
}

static size_t strtab_offset(const char *strtab, const char *name) {
    const char *p = strtab;
    size_t offset = 0;
    while (*p) {
        if (strcmp(p, name) == 0) return offset;
        offset += strlen(p) + 1;
        p += strlen(p) + 1;
    }
    return 0;
}

static size_t sym_index(LabelTable *labels, const char *name) {
    for (size_t i = 0; i < labels->count; i++)
        if (strcmp(labels->entries[i].name, name) == 0) return i + 1;
    return 0;
}

#define ALIGN_UP(x, a) (((x) + (a - 1)) & ~(a - 1))

/* --- Main ELF writer --- */
int write_elf64(const char *filename, struct asm_ret *asmres, LabelTable *labels, RelocTable *relocs) {
    int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return -1; }

    /* --- Build .shstrtab dynamically --- */
    const char *section_names[] = { "", ".text", ".data", ".symtab", ".strtab",
                                    ".shstrtab", ".rela.text", ".rela.data" };
    size_t shstrtab_size = 1; /* initial null byte */
    for (size_t i = 1; i < 8; i++) shstrtab_size += strlen(section_names[i]) + 1;
    char *shstrtab_buf = calloc(1, shstrtab_size);
    size_t off = 1;
    for (size_t i = 1; i < 8; i++) {
        strcpy(&shstrtab_buf[off], section_names[i]);
        off += strlen(section_names[i]) + 1;
    }

    /* --- Build .strtab dynamically --- */
    size_t strtab_size = 1;
    for (size_t i = 0; i < labels->count; i++) strtab_size += strlen(labels->entries[i].name) + 1;
    char *strtab_buf = calloc(1, strtab_size);
    off = 1;
    for (size_t i = 0; i < labels->count; i++) {
        strcpy(&strtab_buf[off], labels->entries[i].name);
        off += strlen(labels->entries[i].name) + 1;
    }

    /* --- Section offsets --- */
    size_t shnum = 8;
    size_t shstrndx = 5;

    size_t offset = sizeof(Elf64_Ehdr); /* we will write shdr later at e_shoff */

    size_t text_offset = ALIGN_UP(offset, 16);
    size_t text_size = asmres->code_size ? asmres->code_size : 1;
    offset = text_offset + text_size;

    size_t data_offset = ALIGN_UP(offset, 8);
    size_t data_size = asmres->data_size ? asmres->data_size : 1;
    offset = data_offset + data_size;

    size_t symtab_offset = ALIGN_UP(offset, 8);
    size_t nsyms = labels->count + 1;
    offset = symtab_offset + nsyms * sizeof(Elf64_Sym);

    size_t strtab_offset = offset;
    offset += strtab_size;

    size_t shstrtab_offset = offset;
    offset += shstrtab_size;

    size_t n_rela_text = 0, n_rela_data = 0;
    for (size_t i = 0; i < relocs->count; i++) {
        if (relocs->entries[i].section == SECTION_CODE) n_rela_text++;
        else n_rela_data++;
    }
    size_t rela_text_offset = ALIGN_UP(offset, 8);
    offset = rela_text_offset + n_rela_text * sizeof(Elf64_Rela);

    size_t rela_data_offset = ALIGN_UP(offset, 8);
    offset = rela_data_offset + n_rela_data * sizeof(Elf64_Rela);

    /* --- ELF header --- */
    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[0] = 0x7f; ehdr.e_ident[1] = 'E'; ehdr.e_ident[2] = 'L'; ehdr.e_ident[3] = 'F';
    ehdr.e_ident[4] = ELFCLASS64;
    ehdr.e_ident[5] = ELFDATA2LSB;
    ehdr.e_ident[6] = EV_CURRENT;
    ehdr.e_ident[7] = ELFOSABI_SYSV;
    ehdr.e_type = ET_REL;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum = shnum;
    ehdr.e_shstrndx = shstrndx;
    ehdr.e_shoff = offset; /* section headers will be written at end */

    if (write(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) { perror("write ehdr"); goto fail; }

    /* --- Write sections --- */
    if (pwrite(fd, asmres->code, text_size, text_offset) < 0) { perror("write .text"); goto fail; }
    if (pwrite(fd, asmres->data, data_size, data_offset) < 0) { perror("write .data"); goto fail; }
    if (pwrite(fd, strtab_buf, strtab_size, strtab_offset) < 0) { perror("write .strtab"); goto fail; }
    if (pwrite(fd, shstrtab_buf, shstrtab_size, shstrtab_offset) < 0) { perror("write .shstrtab"); goto fail; }

    /* --- Symbol table --- */
    Elf64_Sym *syms = calloc(nsyms, sizeof(Elf64_Sym));
    syms[0].st_info = ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE);
    syms[0].st_shndx = SHN_UNDEF;
    off = 1;
    for (size_t i = 0; i < labels->count; i++) {
        syms[i+1].st_name = off;
        syms[i+1].st_info = ELF64_ST_INFO(STB_GLOBAL, labels->entries[i].section == SECTION_CODE ? STT_FUNC : STT_OBJECT);
        syms[i+1].st_other = STV_DEFAULT;
        syms[i+1].st_shndx = labels->entries[i].section == SECTION_CODE ? 1 : 2;
        syms[i+1].st_value = labels->entries[i].address;
        off += strlen(labels->entries[i].name) + 1;
    }
    if (pwrite(fd, syms, nsyms * sizeof(Elf64_Sym), symtab_offset) < 0) { perror("write .symtab"); goto fail; }

    /* --- Relocations --- */
    if (n_rela_text > 0) {
        Elf64_Rela *rela = calloc(n_rela_text, sizeof(Elf64_Rela));
        size_t idx = 0;
        for (size_t i = 0; i < relocs->count; i++) {
            if (relocs->entries[i].section != SECTION_CODE) continue;
            rela[idx].r_offset = relocs->entries[i].offset;
            rela[idx].r_info = ELF64_R_INFO(sym_index(labels, relocs->entries[i].label->name), R_X86_64_32);
            rela[idx].r_addend = 0;
            idx++;
        }
        if (pwrite(fd, rela, n_rela_text * sizeof(Elf64_Rela), rela_text_offset) < 0) { perror("write .rela.text"); goto fail; }
        free(rela);
    }

    if (n_rela_data > 0) {
        Elf64_Rela *rela = calloc(n_rela_data, sizeof(Elf64_Rela));
        size_t idx = 0;
        for (size_t i = 0; i < relocs->count; i++) {
            if (relocs->entries[i].section != SECTION_DATA) continue;
            rela[idx].r_offset = relocs->entries[i].offset;
            rela[idx].r_info = ELF64_R_INFO(sym_index(labels, relocs->entries[i].label->name), R_X86_64_64);
            rela[idx].r_addend = 0;
            idx++;
        }
        if (pwrite(fd, rela, n_rela_data * sizeof(Elf64_Rela), rela_data_offset) < 0) { perror("write .rela.data"); goto fail; }
        free(rela);
    }

    /* --- Section headers --- */
    Elf64_Shdr shdr[8];
    memset(shdr, 0, sizeof(shdr));

    /* Null section */
    shdr[0].sh_type = SHT_NULL;

    /* Helper for offsets in .shstrtab */
    size_t shstr_offsets[8];
    shstr_offsets[0] = 0;
    size_t accum = 1;
    for (size_t i = 1; i < 8; i++) {
        shstr_offsets[i] = accum;
        accum += strlen(section_names[i]) + 1;
    }

    /* .text */
    shdr[1].sh_name = shstr_offsets[1];
    shdr[1].sh_type = SHT_PROGBITS;
    shdr[1].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    shdr[1].sh_offset = text_offset;
    shdr[1].sh_size = text_size;
    shdr[1].sh_addralign = 16;

    /* .data */
    shdr[2].sh_name = shstr_offsets[2];
    shdr[2].sh_type = SHT_PROGBITS;
    shdr[2].sh_flags = SHF_ALLOC | SHF_WRITE;
    shdr[2].sh_offset = data_offset;
    shdr[2].sh_size = data_size;
    shdr[2].sh_addralign = 8;

    /* .symtab */
    shdr[3].sh_name = shstr_offsets[3];
    shdr[3].sh_type = SHT_SYMTAB;
    shdr[3].sh_offset = symtab_offset;
    shdr[3].sh_size = nsyms * sizeof(Elf64_Sym);
    shdr[3].sh_link = 4; /* .strtab */
    shdr[3].sh_info = 1; /* first non-local symbol index */
    shdr[3].sh_addralign = 8;
    shdr[3].sh_entsize = sizeof(Elf64_Sym);

    /* .strtab */
    shdr[4].sh_name = shstr_offsets[4];
    shdr[4].sh_type = SHT_STRTAB;
    shdr[4].sh_offset = strtab_offset;
    shdr[4].sh_size = strtab_size;
    shdr[4].sh_addralign = 1;

    /* .shstrtab */
    shdr[5].sh_name = shstr_offsets[5];
    shdr[5].sh_type = SHT_STRTAB;
    shdr[5].sh_offset = shstrtab_offset;
    shdr[5].sh_size = shstrtab_size;
    shdr[5].sh_addralign = 1;

    /* .rela.text */
    shdr[6].sh_name = shstr_offsets[6];
    shdr[6].sh_type = SHT_RELA;
    shdr[6].sh_offset = rela_text_offset;
    shdr[6].sh_size = n_rela_text * sizeof(Elf64_Rela);
    shdr[6].sh_link = 3; /* symtab */
    shdr[6].sh_info = 1; /* text section index */
    shdr[6].sh_addralign = 8;
    shdr[6].sh_entsize = sizeof(Elf64_Rela);

    /* .rela.data */
    shdr[7].sh_name = shstr_offsets[7];
    shdr[7].sh_type = SHT_RELA;
    shdr[7].sh_offset = rela_data_offset;
    shdr[7].sh_size = n_rela_data * sizeof(Elf64_Rela);
    shdr[7].sh_link = 3; /* symtab */
    shdr[7].sh_info = 2; /* data section index */
    shdr[7].sh_addralign = 8;
    shdr[7].sh_entsize = sizeof(Elf64_Rela);

    if (pwrite(fd, shdr, sizeof(shdr), ehdr.e_shoff) < 0) { perror("write shdr"); goto fail; }

    free(shstrtab_buf);
    free(strtab_buf);
    free(syms);
    close(fd);
    return 0;

fail:
    free(shstrtab_buf);
    free(strtab_buf);
    free(syms);
    close(fd);
    return -1;
}


// int main(void) {
    // /* --- Example machine code: a single 'ret' instruction --- */
    // uint8_t code[] = { 0xc3 }; // ret
    // uint8_t data[] = { 0x42, 0x43, 0x44 }; // some bytes in .data
// 
    // struct asm_ret asmres;
    // asmres.code = code;
    // asmres.code_size = sizeof(code);
    // asmres.data = data;
    // asmres.data_size = sizeof(data);
// 
    // /* --- Labels --- */
    // LabelTable labels;
    // labels.count = 2;
    // labels.capacity = 2;
    // labels.entries = malloc(sizeof(Label) * labels.capacity);
// 
    // labels.entries[0].name = "start";
    // labels.entries[0].address = 0;  // offset in .text
    // labels.entries[0].section = SECTION_CODE;
// 
    // labels.entries[1].name = "mydata";
    // labels.entries[1].address = 0;  // offset in .data
    // labels.entries[1].section = SECTION_DATA;
// 
    // /* --- Relocations --- */
    // RelocTable relocs;
    // relocs.count = 1;
    // relocs.capacity = 1;
    // relocs.entries = malloc(sizeof(Reloc) * relocs.capacity);
// 
    // relocs.entries[0].label = &labels.entries[1]; // point to 'mydata'
    // relocs.entries[0].offset = 0;                 // patch first byte of .text
    // relocs.entries[0].section = SECTION_CODE;
// 
    // /* --- Write ELF file --- */
    // if (write_elf64("test.o", &asmres, &labels, &relocs) != 0) {
        // fprintf(stderr, "Failed to write ELF file\n");
        // free(labels.entries);
        // free(relocs.entries);
        // return 1;
    // }
// 
    // printf("ELF object 'test.o' written successfully.\n");
// 
    // free(labels.entries);
    // free(relocs.entries);
    // return 0;
// }
