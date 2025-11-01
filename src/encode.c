#define _XOPEN_SOURCE 500

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "jasm.h"

// -------------------- Register encoding --------------------
typedef struct {
    const char *name;
    uint8_t code;
} RegMap;

typedef enum {
    STAGE1,
    STAGE2,
} Stage;

Stage stage = STAGE1;

Section cur_section = SECTION_CODE;
size_t* code_pos;
RelocTable G_relocs;

static RegMap regs[] = {
    // 8-bit legacy low
    {"%al",0}, {"%cl",1}, {"%dl",2}, {"%bl",3},
    {"%spl",4}, {"%bpl",5}, {"%sil",6}, {"%dil",7},
    // 8-bit legacy high
    {"%ah",4}, {"%ch",5}, {"%dh",6}, {"%bh",7},
    // 16-bit
    {"%ax",0}, {"%cx",1}, {"%dx",2}, {"%bx",3},
    {"%sp",4}, {"%bp",5}, {"%si",6}, {"%di",7},
    // 32-bit
    {"%eax",0}, {"%ecx",1}, {"%edx",2}, {"%ebx",3},
    {"%esp",4}, {"%ebp",5}, {"%esi",6}, {"%edi",7},
    // 64-bit
    {"%rax",0}, {"%rcx",1}, {"%rdx",2}, {"%rbx",3},
    {"%rsp",4}, {"%rbp",5}, {"%rsi",6}, {"%rdi",7},
    // Extended registers
    {"%r8",8}, {"%r9",9}, {"%r10",10}, {"%r11",11},
    {"%r12",12}, {"%r13",13}, {"%r14",14}, {"%r15",15},
    {"%r8d",8}, {"%r9d",9}, {"%r10d",10}, {"%r11d",11},
    {"%r12d",12}, {"%r13d",13}, {"%r14d",14}, {"%r15d",15},
    {"%r8w",8}, {"%r9w",9}, {"%r10w",10}, {"%r11w",11},
    {"%r12w",12}, {"%r13w",13}, {"%r14w",14}, {"%r15w",15},
    {"%r8b",8}, {"%r9b",9}, {"%r10b",10}, {"%r11b",11},
    {"%r12b",12}, {"%r13b",13}, {"%r14b",14}, {"%r15b",15},
    {NULL,0}
};

static int reg_code(const char *r) {
    if(!r) return -1;
    for(int i=0; regs[i].name; i++)
        if(strcmp(r,regs[i].name)==0) return regs[i].code;
    return -1;
}


static int reg_size(const char *r) {
    if (!r) return 64;
    if (r[0] == '%') r++;
    size_t len = strlen(r);

    // 8-bit new low
    if (r[len-1] == 'l') return 8;  // legacy low?
    if (r[len-1] == 'h') return 8;
    if (r[len-1] == 'b') return 8;
    if (r[len-1] == 'w') return 16;
    if (r[0] == 'e' || r[len-1] == 'd') return 32;
    if (r[0] == 'r') return 64;
    return 16; // else
}

inline static bool check_need_sib(int rm) {

    if (rm == 4 || rm == 12) return true;
    return false;
}

// -------------------- REX prefix --------------------
static void emit_rex(uint8_t **out, bool w, bool r, bool b, bool x) {
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;
    if (r) rex |= 0x04;
    if (x) rex |= 0x02;
    if (b) rex |= 0x01;
    *(*out)++ = rex;
}

static void emit_sib(uint8_t **out, int scale, int index, int base) {
    uint8_t sib = 0x00;
    sib |= (scale << 6);
    if (index == -1) sib |= 4;
    else sib |= (index & 5) << 3;
    sib |= base & 5;
    *(*out)++ = sib;
}



size_t encode_mov_reg_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);
    if (sz != reg_size(src->reg)) {
        fprintf(stderr, "Size mismatch: %s vs %s\n", dst->reg, src->reg);
        return 0;
    }

    int r = reg_code(src->reg);
    int rm = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_r = (r & 8) != 0;
    bool rex_b = (rm & 8) != 0;

    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_r || rex_b ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl") ||
        !strcmp(src->reg, "%sil") || !strcmp(src->reg, "%dil") || !strcmp(src->reg, "%bpl") || !strcmp(src->reg, "%spl");

    bool high = (src->reg[strlen(src->reg)-1] == 'h') || (dst->reg[strlen(dst->reg)-1] == 'h');


    if (high && needs_rex) {
        fprintf(stderr, "High register not supported with REX prefix\n");
        exit(1);
    }

    switch (sz) {
        case 8:
            if (needs_rex && !high) {emit_rex(&out, rex_w, rex_r, rex_b, 0);}
            *out++ = 0x88; // MOV r/m8, r8
            *out++ = (0b11 << 6) | ((r & 7) << 3) | (rm & 7);
            size += needs_rex ? 3 : 2;
            break;
        case 16:
            *out++ = 0x66; // operand-size prefix
            if (needs_rex && !high) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x89; 
            *out++ = (0b11 << 6) | ((r & 7) << 3) | (rm & 7);
            size += 3;
            break;
        case 64:
        case 32:
            if (needs_rex && !high) {emit_rex(&out, rex_w, rex_r, rex_b, 0);}
            *out++ = 0x89;
            *out++ = (0b11 << 6) | ((r & 7) << 3) | (rm & 7);
            size += needs_rex ? 3 : 2;
            break;
    }
    return size;
}

size_t encode_mov_imm_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int sz = reg_size(dst->reg);

    
    int rm = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;



    bool needs_rex = rex_w || rex_b ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");

    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0xB0 + reg_code(dst->reg);
            *out++ = (uint8_t)src->imm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            if (rm > 7) rm -= 8;
            *out++ = 0xB8 + rm;
            *((uint16_t*)out) = (uint16_t)src->imm;
            out += 2;
            size += 4;
            break;
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            if (rm > 7) rm -= 8;
            *out++ = 0xB8 + rm;
            *((uint32_t*)out) = (uint32_t)src->imm;
            out += 4;
            size += 6;
            break;
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            if (rm > 7) rm -= 8;
            *out++ = 0xC7;
            *out++ = (0b11 << 6) | (rm & 7);
            *((uint32_t*)out) = (uint32_t)src->imm;
            out += 4;
            size += 6;
            break;
    }
    return size;
}

size_t encode_mov_reg_mem(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int sz = reg_size(src->reg);

    int src_sz = reg_size(dst->mem.base);

    int rm = reg_code(dst->mem.base);
    int reg = reg_code(src->reg);

    bool rex_w = (sz == 64);
    bool rex_r = (reg & 8) != 0;
    bool rex_b = (rm & 8) != 0;

    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(src->reg, "%sil") || !strcmp(src->reg, "%dil") || !strcmp(src->reg, "%bpl") || !strcmp(src->reg, "%spl");


    bool needs_sib = check_need_sib(rm);


    if (src_sz != 32 && src_sz != 64) {
        fprintf(stderr, "Invalid source size: %d\n", src_sz);
        return 0;
    }

    if (rm > 7) rm -= 8;
    if (reg > 7) reg -= 8;

    if (src_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    
    if (rm == 0b101) {
        // modrm would give rip as base
        switch (sz) {
            case 8:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x88;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
            case 16:
                *out++ = 0x66;
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x89;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 4;
                break;
            case 32:
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x89;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
        }
        return size;
    }

    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x88;
            if (reg > 7) reg -= 8;
            if (rm > 7) rm -= 8;
            *out++ = (reg << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x89;
            if (reg > 7) reg -= 8;
            if (rm > 7) rm -= 8;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x89;
            if (reg > 7) reg -= 8;
            if (rm > 7) rm -= 8;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x89;
            if (reg > 7) reg -= 8;
            if (rm > 7) rm -= 8;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
    }

    if (needs_sib) {
        emit_sib(&out, 0, 0b100, reg_code(dst->mem.base));
        size++;
    }
    return size;
}

size_t encode_mov_mem_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);


    int mem_sz = reg_size(src->mem.base);

    
    int rm = reg_code(src->mem.base);
    int reg = reg_code(dst->reg);

    bool needs_sib = check_need_sib(rm);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;
    bool rex_r = (reg & 8) != 0;



    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");


    if (mem_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    if (rm > 7) rm -= 8;
    if (reg > 7) reg -= 8;

    if (rm == 0b101) {
        // modrm would give rip as base
        switch (sz) {
            case 8:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x8A;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
            case 16:
                *out++ = 0x66;
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x8B;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 4;
                break;
            case 32:
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x8B;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
        }
        return size;
    }



    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x8A;
            *out++ = (reg << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            /* FALLTHRU */
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x8B;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x8B;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
    }
    
    if (needs_sib) {
        emit_sib(&out, 0, 0b100, reg_code(src->mem.base));
        size++;
    }

    return size;
}

size_t encode_mov_mem_imm(uint8_t *out, Operand* dst, Operand* src, Label* label) {
    size_t size = 0;

    int mem_reg_size = reg_size(dst->mem.base);
    int mem_sz = dst->mem.size;

    if (mem_sz == 0) {
        fprintf(stderr, "mov imm, mem: size not specified\n");
        return 0;
    }

    int rm = reg_code(dst->mem.base);


    bool rex_w = (mem_sz == 64);
    bool rex_b = (rm & 8) != 0;

    if (rm > 7) rm -= 8;


    bool needs_rex = rex_w || rex_b;
    if (mem_reg_size == 32) {
        *out++ = 0x67;
        size++;
    }

    switch (mem_sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0xc6;
            
            *out++ = rm;
            *out++ = src->imm;
            size += 3;
            break;
        case 16:
            *out++ = 0x66;
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0xc7;
            *out++ = rm;
            *((uint16_t*)out) = (uint16_t)src->imm;
            out += 2;
            size += 5;
            break;
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0xc7;
            *out++ = rm;
            *((uint32_t*)out) = (uint32_t)src->imm;
            out += 4;
            size += 7;
            break;
        }

    return size;
}

size_t encode_mov_reg_label(uint8_t *out, Operand* dst, Operand* src, LabelTable* label_table) {
    printf("here\n");

    if (label_table == NULL) {
        src->imm = 0;
        return encode_mov_imm_reg(out, dst, src);
    }

    if (reg_size(dst->reg) != 64) {
        fprintf(stderr, "Invalid size: %s\n", dst->reg);
        return 0;
    }


    for (size_t i = 0; i < G_labels->count; i++) {
        if (!strcmp(G_labels->entries[i].name, src->labelref)) {
            if (args->outformat == OF_BINARY) {
                src->imm = G_labels->entries[i].address;
                return encode_mov_imm_reg(out, dst, src);
            }

            if (args->outformat == OF_ELF) {
                Label* l = &G_labels->entries[i];

                printf("Label %s is in section %d, but current section is %d\n", l->name, l->section, cur_section);
                printf("emitting reloc\n");
                size_t size = 0;
                int sz = reg_size(dst->reg);
                int rm = reg_code(dst->reg);
                bool rex_w = (sz == 64);
                bool rex_b = (rm & 8) != 0;
                bool needs_rex = rex_w || rex_b;
                if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
                if (rm > 7) rm -= 8;
                *out++ = 0xC7;
                *out++ = (0b11 << 6) | (rm & 7);
                *((uint32_t*)out) = 0;
                out += 4;
                size += 6;
                printf("size: %#x\n", (uint32_t)size-4 + *(uint32_t*)(code_pos));
                if (stage == STAGE2)
                    emit_reloc(&G_relocs, l, (uint32_t)size-4 + *(uint32_t*)(code_pos), SECTION_CODE);
                return size;
            }
        }
    }

    fprintf(stderr, "Label not found: %s\n", src->labelref);
    return 0;
}

size_t encode_add_reg_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);
    if (sz != reg_size(src->reg)) {
        fprintf(stderr, "Size mismatch: %s vs %s\n", dst->reg, src->reg);
        return 0;
    }

    int reg = reg_code(src->reg);
    int rm = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_r = (reg & 8) != 0;
    bool rex_b = (rm & 8) != 0;

    if (reg > 7) reg -= 8;
    if (rm > 7) rm -= 8;

    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_r || rex_b ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");


    bool high = (src->reg[strlen(src->reg)-1] == 'h') || (dst->reg[strlen(dst->reg)-1] == 'h');


    if (high && needs_rex) {
        fprintf(stderr, "High register not supported with REX prefix\n");
        exit(1);
    }

    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0);}
            *out++ = 0x00;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += needs_rex ? 3 : 2;
            break;
        case 16:
            *out++ = 0x66; // operand-size prefix
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x01;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += 3;
            break;
        case 64:
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x01;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += 2;
            break;
    }

    return size;
}

size_t encode_add_imm_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int sz = reg_size(dst->reg);

    int rm = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;

    if (rm > 7) rm -= 8;
    
    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_b ||
    !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");
    
    if (rm == 0) {
        switch(sz) {
            case 8:
                *out++ = 0x04;
                *out++ = src->imm;
                size += 2;
                break;
            case 16:
                *out++ = 0x66;
                *out++ = 0x05;
                *((uint16_t*)out) = (uint16_t)src->imm;
                out += 2;
                size += 4;
                break;
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
                // FALLTHRU
            case 32:
                *out++ = 0x05;
                *((uint32_t*)out) = (uint32_t)src->imm;
                out += 4;
                size += 6;
                break;
        }
        return size;
    }

    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x80;
            *out++ = (0b11 << 6) | (rm & 7);
            *out++ = src->imm;
            size += 3;
            break;
        case 16:
            *out++ = 0x66;
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (0b11 << 6) | (rm & 7);
            *((uint16_t*)out) = (uint16_t)src->imm;
            out += 2;
            size += 5;
            break;
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (0b11 << 6) | (rm & 7);
            *((uint32_t*)out) = (uint32_t)src->imm;
            out += 4;
            size += 7;
            break;

    }

    return size;
}

size_t encode_add_imm_mem(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int mem_reg_sz = reg_size(dst->mem.base);

    int mem_size = dst->mem.size;

    int rm = reg_code(dst->mem.base);

    bool rex_w = (mem_size == 64);
    bool rex_b = (rm & 8) != 0;

    if (rm > 7) rm -= 8;

    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_b;
    if (mem_reg_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    switch (mem_size) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x80;
            *out++ = (rm & 7);
            *out++ = src->imm;
            size += 3;
            break;
        case 16:
            *out++ = 0x66;
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (rm & 7);
            *((uint16_t*)out) = (uint16_t)src->imm;
            out += 2;
            size += 5;
            break;
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (rm & 7);
            *((uint32_t*)out) = (uint32_t)src->imm;
            out += 4;
            size += 7;
            break;
    }

    return size;
}

static size_t encode_add_reg_mem(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int mem_reg_sz = reg_size(dst->mem.base);

    int mem_size = reg_size(src->reg);

    if (mem_reg_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    int mod = reg_code(src->reg);
    int rm = reg_code(dst->mem.base);

    bool rex_w = (mem_size == 64);
    bool rex_b = (rm & 8) != 0;
    bool rex_r = (mod & 8) != 0;

    bool high = (src->reg[strlen(src->reg)-1] == 'h');

    bool needs_sib = check_need_sib(rm);


    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(src->reg, "%sil") || !strcmp(src->reg, "%dil") || !strcmp(src->reg, "%bpl") || !strcmp(src->reg, "%spl");

    if (high && needs_rex) {
        fprintf(stderr, "High register not supported with REX prefix\n");
        exit(1);
    }

    if (mod > 7) mod -= 8;
    if (rm > 7) rm -= 8;


    if (rm == 0b101) {
        // modrm would give rip as base
        switch (mem_size) {
            case 8:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x00;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
            case 16:
                *out++ = 0x66;
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x01;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 4;
                break;
            case 32:
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x01;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
        }
        return size;
    }




    switch (mem_size) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x00;
            *out++ = (mod << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            size++;
            // FALLTHRU
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x01;
            *out++ = (mod << 3) | rm;
            // if (needs_sib) {emit_sib(&out, 0, 0b100, reg_code(dst->mem.base)); size++;}
            size += 2;
            break;

    }


    if (needs_sib) {
        emit_sib(&out, 0, 0b100, reg_code(dst->mem.base));
        size++;
    }


    return size;
}

size_t encode_add_mem_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);


    int mem_sz = reg_size(src->mem.base);

    
    int rm = reg_code(src->mem.base);
    int reg = reg_code(dst->reg);

    bool needs_sib = check_need_sib(rm);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;
    bool rex_r = (reg & 8) != 0;



    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");


    if (mem_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    if (rm > 7) rm -= 8;
    if (reg > 7) reg -= 8;

    if (rm == 0b101) {
        // modrm would give rip as base
        switch (sz) {
            case 8:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x02;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
            case 16:
                *out++ = 0x66;
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x03;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 4;
                break;
            case 32:
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x03;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
        }
        return size;
    }



    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x02;
            *out++ = (reg << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            /* FALLTHRU */
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x03;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x03;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
    }
    
    if (needs_sib) {
        emit_sib(&out, 0, 0b100, reg_code(src->mem.base));
        size++;
    }

    return size;
}


// -------------------- Subtraction --------------------
size_t encode_sub_reg_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);
    if (sz != reg_size(src->reg)) {
        fprintf(stderr, "Size mismatch: %s vs %s\n", dst->reg, src->reg);
        return 0;
    }

    int reg = reg_code(src->reg);
    int rm = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_r = (reg & 8) != 0;
    bool rex_b = (rm & 8) != 0;

    if (reg > 7) reg -= 8;
    if (rm > 7) rm -= 8;

    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_r || rex_b ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");


    bool high = (src->reg[strlen(src->reg)-1] == 'h') || (dst->reg[strlen(dst->reg)-1] == 'h');


    if (high && needs_rex) {
        fprintf(stderr, "High register not supported with REX prefix\n");
        exit(1);
    }

    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0);}
            *out++ = 0x28;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += needs_rex ? 3 : 2;
            break;
        case 16:
            *out++ = 0x66; // operand-size prefix
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x29;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += 3;
            break;
        case 64:
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x29;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += 2;
            break;
    }

    return size;
}

size_t encode_sub_imm_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int sz = reg_size(dst->reg);

    int rm = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;

    if (rm > 7) rm -= 8;
    
    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_b ||
    !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");
    
    if (rm == 0) {
        switch(sz) {
            case 8:
                *out++ = 0x2c;
                *out++ = src->imm;
                size += 2;
                break;
            case 16:
                *out++ = 0x66;
                *out++ = 0x2d;
                *((uint16_t*)out) = (uint16_t)src->imm;
                out += 2;
                size += 4;
                break;
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
                // FALLTHRU
            case 32:
                *out++ = 0x2d;
                *((uint32_t*)out) = (uint32_t)src->imm;
                out += 4;
                size += 6;
                break;
        }
        return size;
    }

    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x80;
            *out++ = (0b11 << 6) | (5 << 3) | (rm & 7);
            *out++ = src->imm;
            size += 3;
            break;
        case 16:
            *out++ = 0x66;
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (0b11 << 6) | (5 << 3) | (rm & 7);
            *((uint16_t*)out) = (uint16_t)src->imm;
            out += 2;
            size += 5;
            break;
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (0b11 << 6) | (5 << 3) | (rm & 7);
            *((uint32_t*)out) = (uint32_t)src->imm;
            out += 4;
            size += 7;
            break;

    }

    return size;
}

size_t encode_sub_imm_mem(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int mem_reg_sz = reg_size(dst->mem.base);

    int mem_size = dst->mem.size;

    int rm = reg_code(dst->mem.base);

    bool rex_w = (mem_size == 64);
    bool rex_b = (rm & 8) != 0;

    if (rm > 7) rm -= 8;

    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_b;
    if (mem_reg_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    switch (mem_size) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x80;
            *out++ = (5 << 3) | (rm & 7);
            *out++ = src->imm;
            size += 3;
            break;
        case 16:
            *out++ = 0x66;
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (5 << 3) | (rm & 7);
            *((uint16_t*)out) = (uint16_t)src->imm;
            out += 2;
            size += 5;
            break;
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (5 << 3) | (rm & 7);
            *((uint32_t*)out) = (uint32_t)src->imm;
            out += 4;
            size += 7;
            break;
    }

    return size;
}

static size_t encode_sub_reg_mem(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int mem_reg_sz = reg_size(dst->mem.base);

    int mem_size = reg_size(src->reg);

    if (mem_reg_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    int mod = reg_code(src->reg);
    int rm = reg_code(dst->mem.base);

    bool rex_w = (mem_size == 64);
    bool rex_b = (rm & 8) != 0;
    bool rex_r = (mod & 8) != 0;

    bool high = (src->reg[strlen(src->reg)-1] == 'h');


    bool needs_sib = check_need_sib(rm);


    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(src->reg, "%sil") || !strcmp(src->reg, "%dil") || !strcmp(src->reg, "%bpl") || !strcmp(src->reg, "%spl");

    if (high && needs_rex) {
        fprintf(stderr, "High register not supported with REX prefix\n");
        exit(1);
    }

    if (mod > 7) mod -= 8;
    if (rm > 7) rm -= 8;


    if (rm == 0b101) {
        // modrm would give rip as base
        switch (mem_size) {
            case 8:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x28;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
            case 16:
                *out++ = 0x66;
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x29;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 4;
                break;
            case 32:
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x29;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
        }
        return size;
    }




    switch (mem_size) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x28;
            *out++ = (mod << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            size++;
            // FALLTHRU
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x29;
            *out++ = (mod << 3) | rm;
            // if (needs_sib) {emit_sib(&out, 0, 0b100, reg_code(dst->mem.base)); size++;}
            size += 2;
            break;

    }


    if (needs_sib) {
        emit_sib(&out, 0, 0b100, reg_code(dst->mem.base));
        size++;
    }


    return size;
}

size_t encode_sub_mem_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);


    int mem_sz = reg_size(src->mem.base);

    
    int rm = reg_code(src->mem.base);
    int reg = reg_code(dst->reg);

    bool needs_sib = check_need_sib(rm);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;
    bool rex_r = (reg & 8) != 0;



    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");


    if (mem_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    if (rm > 7) rm -= 8;
    if (reg > 7) reg -= 8;

    if (rm == 0b101) {
        // modrm would give rip as base
        switch (sz) {
            case 8:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x2A;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
            case 16:
                *out++ = 0x66;
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x2B;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 4;
                break;
            case 32:
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x2B;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
        }
        return size;
    }



    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x2A;
            *out++ = (reg << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            /* FALLTHRU */
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x2B;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x2B;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
    }
    
    if (needs_sib) {
        emit_sib(&out, 0, 0b100, reg_code(src->mem.base));
        size++;
    }

    return size;
}


// -------------------- CMP --------------------

size_t encode_cmp_reg_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);
    if (sz != reg_size(src->reg)) {
        fprintf(stderr, "Size mismatch: %s vs %s\n", dst->reg, src->reg);
        return 0;
    }

    int reg = reg_code(src->reg);
    int rm = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_r = (reg & 8) != 0;
    bool rex_b = (rm & 8) != 0;

    if (reg > 7) reg -= 8;
    if (rm > 7) rm -= 8;

    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_r || rex_b ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");


    bool high = (src->reg[strlen(src->reg)-1] == 'h') || (dst->reg[strlen(dst->reg)-1] == 'h');


    if (high && needs_rex) {
        fprintf(stderr, "High register not supported with REX prefix\n");
        exit(1);
    }

    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0);}
            *out++ = 0x38;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += needs_rex ? 3 : 2;
            break;
        case 16:
            *out++ = 0x66; // operand-size prefix
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x39;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += 3;
            break;
        case 64:
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x39;
            *out++ = (0b11 << 6) | ((reg & 7) << 3) | (rm & 7);
            size += 2;
            break;
    }

    return size;
}

size_t encode_cmp_imm_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int sz = reg_size(dst->reg);

    int rm = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;

    if (rm > 7) rm -= 8;
    
    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_b ||
    !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");
    
    if (rm == 0) {
        switch(sz) {
            case 8:
                *out++ = 0x3c;
                *out++ = src->imm;
                size += 2;
                break;
            case 16:
                *out++ = 0x66;
                *out++ = 0x3d;
                *((uint16_t*)out) = (uint16_t)src->imm;
                out += 2;
                size += 4;
                break;
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
                // FALLTHRU
            case 32:
                *out++ = 0x3d;
                *((uint32_t*)out) = (uint32_t)src->imm;
                out += 4;
                size += 6;
                break;
        }
        return size;
    }

    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x80;
            *out++ = (0b11 << 6) | (7 << 3) | (rm & 7);
            *out++ = src->imm;
            size += 3;
            break;
        case 16:
            *out++ = 0x66;
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (0b11 << 6) | (7 << 3) | (rm & 7);
            *((uint16_t*)out) = (uint16_t)src->imm;
            out += 2;
            size += 5;
            break;
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
            *out++ = 0x81;
            *out++ = (0b11 << 6) | (7 << 3) | (rm & 7);
            *((uint32_t*)out) = (uint32_t)src->imm;
            out += 4;
            size += 7;
            break;

    }

    return size;
}

size_t encode_cmp_imm_mem(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    
    int mem_reg_sz = reg_size(dst->mem.base);
    
    int mem_size = dst->mem.size;

    if (!mem_size) {
        fprintf(stderr, "Invalid memory size: %d\n", mem_size);
        return 0;
    }
    
    int rm = reg_code(dst->mem.base);
    
    bool rex_w = (mem_size == 64);
    bool rex_b = (rm & 8) != 0;
    
    if (rm > 7) rm -= 8;
    
    // force REX for new 8-bit registers
    bool needs_rex = rex_w || rex_b;
    if (mem_reg_sz == 32) {
        *out++ = 0x67;
        size++;
    }
    
    switch (mem_size) {
        case 8:
        if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
        *out++ = 0x80;
        *out++ = (7 << 3) | (rm & 7);
        *out++ = src->imm;
        size += 3;
        break;
        case 16:
        *out++ = 0x66;
        if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
        *out++ = 0x81;
        *out++ = (7 << 3) | (rm & 7);
        *((uint16_t*)out) = (uint16_t)src->imm;
        out += 2;
        size += 5;
        break;
        case 32:
        case 64:
        if (needs_rex) {emit_rex(&out, rex_w, 0, rex_b, 0); size++;}
        *out++ = 0x81;
        *out++ = (7 << 3) | (rm & 7);
        *((uint32_t*)out) = (uint32_t)src->imm;
        out += 4;
        size += 7;
        break;
    }
    
    return size;
}

static size_t encode_cmp_reg_mem(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int mem_reg_sz = reg_size(dst->mem.base);

    int mem_size = reg_size(src->reg);

    if (mem_reg_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    int mod = reg_code(src->reg);
    int rm = reg_code(dst->mem.base);

    bool rex_w = (mem_size == 64);
    bool rex_b = (rm & 8) != 0;
    bool rex_r = (mod & 8) != 0;

    bool high = (src->reg[strlen(src->reg)-1] == 'h');


    bool needs_sib = check_need_sib(rm);


    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(src->reg, "%sil") || !strcmp(src->reg, "%dil") || !strcmp(src->reg, "%bpl") || !strcmp(src->reg, "%spl");

    if (high && needs_rex) {
        fprintf(stderr, "High register not supported with REX prefix\n");
        exit(1);
    }

    if (mod > 7) mod -= 8;
    if (rm > 7) rm -= 8;


    if (rm == 0b101) {
        // modrm would give rip as base
        switch (mem_size) {
            case 8:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x38;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
            case 16:
                *out++ = 0x66;
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x39;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 4;
                break;
            case 32:
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x39;
                *out++ = (0b01 << 6) | (mod << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
        }
        return size;
    }



    switch (mem_size) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x38;
            *out++ = (mod << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            size++;
            // FALLTHRU
        case 32:
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x39;
            *out++ = (mod << 3) | rm;
            // if (needs_sib) {emit_sib(&out, 0, 0b100, reg_code(dst->mem.base)); size++;}
            size += 2;
            break;

    }


    if (needs_sib) {
        emit_sib(&out, 0, 0b100, reg_code(dst->mem.base));
        size++;
    }


    return size;
}

size_t encode_cmp_mem_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);


    int mem_sz = reg_size(src->mem.base);

    
    int rm = reg_code(src->mem.base);
    int reg = reg_code(dst->reg);

    bool needs_sib = check_need_sib(rm);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;
    bool rex_r = (reg & 8) != 0;



    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");


    if (mem_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    if (rm > 7) rm -= 8;
    if (reg > 7) reg -= 8;

    if (rm == 0b101) {
        // modrm would give rip as base
        switch (sz) {
            case 8:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x3A;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
            case 16:
                *out++ = 0x66;
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x3B;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 4;
                break;
            case 32:
            case 64:
                if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
                *out++ = 0x3B;
                *out++ = (0b01 << 6) | (reg << 3) | 5;
                *out++ = 0x00;
                size += 3;
                break;
        }
        return size;
    }



    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x3A;
            *out++ = (reg << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            /* FALLTHRU */
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x3B;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x3B;
            *out++ = (reg << 3) | rm;
            size += 2;
            break;
    }
    
    if (needs_sib) {
        emit_sib(&out, 0, 0b100, reg_code(src->mem.base));
        size++;
    }

    return size;
}


// jmp

static size_t encode_rel_jmps(uint8_t *out, Operand* dst, size_t pos, LabelTable *label_table, uint8_t opcode, bool opcode16) {
    // im only supporting 32 bit rel jmps

    size_t size = 0;

    if (!label_table) {
        return 5;
    }
    
    Label* l = NULL;
    
    

    for (l = label_table->entries; l < label_table->entries + label_table->count; l++) {
        if (strcmp(l->name, dst->labelref) == 0)
            break;
    }

    
    if (!l) {
        fprintf(stderr, "Label not found: %s\n", dst->labelref);
        return 0;
    }
    


    int32_t offset = l->address - pos - (opcode16 ? 6 : 5);


    if (opcode16) {
        *out++ = 0x0F;
        size++;
    }
    *out++ = opcode & 0xFF;

    *((int32_t*)out) = (int32_t)offset;
    out += 4;
    size += 5;

    return size;
}

static size_t encode_abs_jmp_reg(uint8_t *out, Operand* dst) {
    size_t size = 0;

    int reg = reg_code(dst->reg);

    bool rex_b = (reg & 8);

    
    // force REX for new 8-bit registers
    bool needs_rex = rex_b;
    
    if (reg > 7) reg -= 8;
    if (needs_rex) {
        emit_rex(&out, 0, 0, rex_b, 0);
        size++;
    }

    *out++ = 0xFF;
    *out++ = (0b11 << 6) | (4 << 3) |  (reg);

    size += 2;

    return size;
}

static size_t encode_abs_jmp_mem(uint8_t *out, Operand* dst) {
    size_t size = 0;

    int mem_reg_sz = reg_size(dst->mem.base);

    int rm = reg_code(dst->mem.base);

    bool rex_r = (rm & 8);

    bool needs_rex = rex_r;

    bool needs_sib = check_need_sib(rm);


    if (mem_reg_sz == 32) {
        *out++ = 0x67;
        size++;
    }

    if (rm > 7) rm -= 8;
    if (needs_rex) {
        emit_rex(&out, 0, 0, rex_r, 0);
        size++;
    }

    *out++ = 0xFF;
    if (rm == 0b101) {
        *out++ = (0b01 << 6) | (4 << 3) | 5;
        *out++ = 0x00;
        size += 3;
        return size;
    }
    *out++ = (4 << 3) |  (rm);

    size += 2;

    if (needs_sib) {
        emit_sib(&out, 0, 0b100, rm);
        size++;
    }

    return size;
}

// int

static size_t encode_int(uint8_t *out, Operand* dst) {
    int imm = dst->imm;

    if (imm == 3) {
        *out++ = 0xCC;
        return 1;
    }
    
    if (imm == 1) {
        *out++ = 0xF1;
        return 1;
    }

    if (imm <= 0xFF) {
        *out++ = 0xCD;
        *out++ = imm;
        return 2;
    }
    return 0;
}

// -------------------- Strip suffix --------------------
static void base_opcode(const char *opc,char *buf){
    size_t len=strlen(opc);
    strcpy(buf,opc);
    if (!strcmp(opc, "sub") || !strcmp(opc, "syscall"))
        return;
    if(len>1){
        char last=opc[len-1];
        if(last=='b'||last=='w'||last=='l'||last=='q') buf[len-1]=0;
    }
}


// -------------------- Encode instruction --------------------
size_t encode_instruction(uint8_t *out, Instruction *inst, __attribute__((unused)) Program *prog, size_t pos, LabelTable *label_table) {
    if (!inst) return 0;
    char opc[32] = {0}; base_opcode(inst->opcode, opc);

    if (inst->noperands == 2) {
        Operand *dst = inst->operands[1];
        Operand *src = inst->operands[0];


        if (strcmp(opc, "mov") == 0) {
            printf("mov\n");
            if (src->kind == OP_REG && dst->kind == OP_REG) return encode_mov_reg_reg(out, dst, src);
            if (src->kind == OP_IMM && dst->kind == OP_REG) return encode_mov_imm_reg(out, dst, src);
            if (dst->kind == OP_MEM && src->kind == OP_REG) return encode_mov_reg_mem(out, dst, src);
            if (dst->kind == OP_REG && src->kind == OP_MEM) return encode_mov_mem_reg(out, dst, src);
            if (dst->kind == OP_MEM && src->kind == OP_IMM) return encode_mov_mem_imm(out, dst, src, NULL);
            if (dst->kind == OP_REG && src->kind == OP_LABELREF) return encode_mov_reg_label(out, dst, src, label_table);
        }
        if (strcmp(opc, "add") == 0) {
            printf("add\n");
            if (src->kind == OP_REG && dst->kind == OP_REG) return encode_add_reg_reg(out, dst, src);
            if (src->kind == OP_IMM && dst->kind == OP_REG) return encode_add_imm_reg(out, dst, src);
            if (src->kind == OP_IMM && dst->kind == OP_MEM) return encode_add_imm_mem(out, dst, src);
            if (src->kind == OP_REG && dst->kind == OP_MEM) return encode_add_reg_mem(out, dst, src);
            if (src->kind == OP_MEM && dst->kind == OP_REG) return encode_add_mem_reg(out, dst, src);
        }
        if (strcmp(opc, "sub") == 0) {
            printf("sub\n");
            if (src->kind == OP_REG && dst->kind == OP_REG) return encode_sub_reg_reg(out, dst, src);
            if (src->kind == OP_IMM && dst->kind == OP_REG) return encode_sub_imm_reg(out, dst, src);
            if (src->kind == OP_IMM && dst->kind == OP_MEM) return encode_sub_imm_mem(out, dst, src);
            if (src->kind == OP_REG && dst->kind == OP_MEM) return encode_sub_reg_mem(out, dst, src);
            if (src->kind == OP_MEM && dst->kind == OP_REG) return encode_sub_mem_reg(out, dst, src);
        }
        if (strcmp(opc, "cmp") == 0) {
            printf("cmp\n");
            if (src->kind == OP_REG && dst->kind == OP_REG) return encode_cmp_reg_reg(out, dst, src);
            if (src->kind == OP_IMM && dst->kind == OP_REG) return encode_cmp_imm_reg(out, dst, src);
            if (src->kind == OP_IMM && dst->kind == OP_MEM) return encode_cmp_imm_mem(out, dst, src);
            if (src->kind == OP_REG && dst->kind == OP_MEM) return encode_cmp_reg_mem(out, dst, src);
            if (src->kind == OP_MEM && dst->kind == OP_REG) return encode_cmp_mem_reg(out, dst, src);
        }
    }

    if (inst->noperands == 1) {
        Operand* op = inst->operands[0];
        if (strncmp(opc, "jmp", 3) == 0 && inst->noperands == 1) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0xE9, false);
            if (op->kind == OP_REG) return encode_abs_jmp_reg(out, op);
            if (op->kind == OP_MEM) return encode_abs_jmp_mem(out, op);
        }
        if (!strcmp(opc, "ja") || !strcmp(opc, "jnbe")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x87, true);
        }
        if (!strcmp(opc, "jae") || !strcmp(opc, "jnb") || !strcmp(opc, "jnc")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x83, true);
        }
        if (!strcmp(opc, "jb") || !strcmp(opc, "jc") || !strcmp(opc, "jnae")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x82, true);
        }
        if (!strcmp(opc, "jbe") || !strcmp(opc, "jna")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x86, true);
        }
        if (!strcmp(opc, "je") || !strcmp(opc, "jz")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x84, true);
        }
        if (!strcmp(opc, "jg") || !strcmp(opc, "jnle")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x8F, true);
        }
        if (!strcmp(opc, "jge") || !strcmp(opc, "jnl")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x8D, true);
        }
        if (!strcmp(opc, "jl") || !strcmp(opc, "jnge")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x8C, true);
        }
        if (!strcmp(opc, "jle") || !strcmp(opc, "jng")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x8E, true);
        }
        if (!strcmp(opc, "jno")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x81, true);
        }
        if (!strcmp(opc, "jnp") || !strcmp(opc, "jpo")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x8B, true);
        }
        if (!strcmp(opc, "jns")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x89, true);
        }
        if (!strcmp(opc, "jnz") || !strcmp(opc, "jne")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x85, true);
        }
        if (!strcmp(opc, "jo")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x80, true);
        }
        if (!strcmp(opc, "jp") || !strcmp(opc, "jpe")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x8A, true);
        }
        if (!strcmp(opc, "js")) {
            if (op->kind == OP_LABELREF) return encode_rel_jmps(out, op, pos, label_table, 0x88, true);
        }
        if (!strcmp(opc, "int")) {
            if (op->kind == OP_IMM) return encode_int(out, op);
        }
    }

    if (inst->noperands == 0) {
        if (!strcmp(opc, "syscall")) {
            *out++ = 0x0f;
            *out++ = 0x05;
            return 2;
        }
    }


    fprintf(stderr, "Unsupported: %s\n", opc);
    return 0;
}

size_t calculate_instruction_size(Instruction *inst) {
    if (!inst) return 0;

    uint8_t out[32] = {0};
    size_t len = encode_instruction(out, inst, NULL, 0, NULL);

    return len;
}

size_t encode_directive(uint8_t* out, Directive* directive) {
    if (!directive) return 0;

    char* name = directive->name;


    if (!strcmp(name, ".string")) {
        if (!directive->args[0]) return 0;
        size_t len = strlen(directive->args[0]);

        snprintf((char*)out, len+1, directive->args[0]);
        return len + 1;
    } if (!strcmp(name, ".data")) {
        cur_section = SECTION_DATA;
        return 0;
    } if (!strcmp(name, ".code")) {
        cur_section = SECTION_CODE;
        return 0;
    }

    return 0;
}



// -------------------- Compute instruction offsets --------------------
static LabelTable* compute_offsets(Program *prog) {
    LabelTable* const label_table = malloc(sizeof(LabelTable));
    label_table->entries = NULL;
    label_table->count = 0;
    label_table->capacity = 0;

    size_t code_off = 0;
    size_t data_off = 0;



    if (!prog) return NULL;


    size_t off = 0;
    for (size_t i = 0; i < prog->nnodes; i++) {
        Node *n = prog->nodes[i];
        if (n->kind == NODE_INSTRUCTION) {
            off += calculate_instruction_size(&n->u.instruction);
            if (cur_section == SECTION_CODE) code_off += off;
            if (cur_section == SECTION_DATA) data_off += off;
        } else if (n->kind == NODE_LABEL) {
            if (label_table->count >= label_table->capacity) {
                label_table->capacity = label_table->capacity ? label_table->capacity * 2 : 128;
                label_table->entries = realloc(label_table->entries, label_table->capacity * sizeof(Label));
                if (!label_table->entries) { perror("realloc"); exit(1); }
            }
            Label *l = label_table->entries + label_table->count++;
            l->name = strdup(n->u.label);
            if (cur_section == SECTION_CODE) l->address = code_off;
            if (cur_section == SECTION_DATA) l->address = data_off;
            l->section = cur_section;
        } else if (n->kind == NODE_DIRECTIVE) {
            printf("Directive: %s\n", n->u.directive.name);
            if (strcmp(n->u.directive.name, ".data") == 0) {
                cur_section = SECTION_DATA;
            } else if (strcmp(n->u.directive.name, ".code") == 0) {
                cur_section = SECTION_CODE;
            } else if (!strcmp(n->u.directive.name, ".org")) {
                if (!n->u.directive.args[0]) continue;
                uint32_t addr = strtoul(n->u.directive.args[0], NULL, 0);
                off = addr;
            } else {
                uint8_t buf[32] = {0};
                size_t len = encode_directive(buf, &n->u.directive);
                printf("string len = %lu\n", len);
                if (cur_section == SECTION_CODE) code_off += len;
                if (cur_section == SECTION_DATA) data_off += len;
            }
        }
    }
    for (size_t i = 0; i < label_table->count; i++) {
        Label *l = label_table->entries + i;
        printf("label: %s, address: %d\n", l->name, l->address);
    }

    G_labels = label_table;

    stage = STAGE2;

    return label_table;
}


LabelTable* G_labels = NULL;


// -------------------- Assemble program --------------------
struct asm_ret* assemble_program(Program *prog) {
    struct asm_ret* ret = malloc(sizeof(struct asm_ret));

    if(!prog) return NULL;
    LabelTable *label_table = compute_offsets(prog);

    init_reloc_table(&G_relocs);


    // FILE* f = fopen("test", "wb");
    // if (!f) { perror("fopen"); exit(1); }

    uint8_t buf[32] = {0};
    code_pos = malloc(sizeof(size_t));

    size_t* data_pos;
    data_pos = (args->outformat == OF_BINARY) ? code_pos : malloc(sizeof(size_t));

    *code_pos = *data_pos = 0;


    uint8_t* code = malloc(prog->nnodes * 32);
    uint8_t* data;
    data = (args->outformat == OF_BINARY) ? code : malloc(prog->nnodes * 32);
    ret->code = code;
    ret->data = data;

    
    for(size_t i=0;i<prog->nnodes;i++){
        Node *n=prog->nodes[i];
        if(n->kind == NODE_INSTRUCTION) {
            size_t len=encode_instruction(buf,&n->u.instruction,prog,*code_pos,label_table);
            if(len==0){ fprintf(stderr,"Skipping unsupported: %s\n",n->u.instruction.opcode); continue;}
            // fwrite(buf, 1, len, f);
            memcpy(code + *code_pos, buf, len);
            *code_pos += len;
        } else if (n->kind == NODE_DIRECTIVE) {
            uint8_t buf[32] = {0};
            size_t len = encode_directive(buf, &n->u.directive);
            printf("dirlen: %lu\n", len);
            printf("data_pos: %lu\n", *data_pos);

            memcpy(data + *data_pos, buf, len);
            *data_pos += len;
        } 
    }

    // fclose(f);

    ret->code_size = (*code_pos);
    ret->data_size = (*data_pos);
    return ret;
}