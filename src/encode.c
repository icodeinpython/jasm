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

// -------------------- REX prefix --------------------
static void emit_rex(uint8_t **out, bool w, bool r, bool b, bool x) {
    uint8_t rex = 0x40;
    if (w) rex |= 0x08;
    if (r) rex |= 0x04;
    if (x) rex |= 0x02;
    if (b) rex |= 0x01;
    *(*out)++ = rex;
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
            *out++ = 0xB8 + rm;
            *((uint64_t*)out) = (uint64_t)src->imm;
            out += 8;
            size += 10;
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
    bool rex_b = (reg & 8) != 0;
    bool rex_r = (rm & 8) != 0;

    bool needs_rex = rex_w || rex_b ||
        !strcmp(src->reg, "%sil") || !strcmp(src->reg, "%dil") || !strcmp(src->reg, "%bpl") || !strcmp(src->reg, "%spl");


    if (src_sz != 32 && src_sz != 64) {
        fprintf(stderr, "Invalid source size: %d\n", src_sz);
        return 0;
    }


    if (src_sz == 32) {
        *out++ = 0x67;
        size++;
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
    return size;
}

size_t encode_mov_mem_reg(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;
    int sz = reg_size(dst->reg);

    printf("reg_sz: %d\n", sz);

    int mem_sz = reg_size(src->mem.base);

    
    int rm = reg_code(src->mem.base);
    int reg = reg_code(dst->reg);

    bool rex_w = (sz == 64);
    bool rex_b = (rm & 8) != 0;
    bool rex_r = (reg & 8) != 0;


    bool needs_rex = rex_w || rex_b || rex_r ||
        !strcmp(dst->reg, "%sil") || !strcmp(dst->reg, "%dil") || !strcmp(dst->reg, "%bpl") || !strcmp(dst->reg, "%spl");


    if (mem_sz == 32) {
        *out++ = 0x67;
        size++;
    }


    switch (sz) {
        case 8:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x8A;
            if (reg > 7) reg -= 8;
            if (rm > 7) rm -= 8;
            *out++ = (reg << 3) | rm;
            size += 2;
            break;
        case 16:
            *out++ = 0x66;
            /* FALLTHRU */
        case 32:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x8B;
            if (reg > 7) reg -= 8;
            if (rm > 7) rm -= 8;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
        case 64:
            if (needs_rex) {emit_rex(&out, rex_w, rex_r, rex_b, 0); size++;}
            *out++ = 0x8B;
            if (reg > 7) reg -= 8;
            if (rm > 7) rm -= 8;
            *out++ = (reg << 3) | rm;
            size += 3;
            break;
    }

    return size;
}

size_t encode_mov_mem_imm(uint8_t *out, Operand* dst, Operand* src) {
    size_t size = 0;

    int mem_reg_size = reg_size(dst->mem.base);
    int mem_sz = dst->mem.size;

    if (mem_sz == 0) {
        fprintf(stderr, "mov imm, mem: size not specified\n");
        return 0;
    }

    int rm = reg_code(dst->mem.base);

    printf("mem base: %s\n", dst->mem.base);

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


// -------------------- Strip suffix --------------------
static void base_opcode(const char *opc,char *buf){
    size_t len=strlen(opc);
    strcpy(buf,opc);
    if(len>1){
        char last=opc[len-1];
        if(last=='b'||last=='w'||last=='l'||last=='q') buf[len-1]=0;
    }
}

// -------------------- Encode instruction --------------------
size_t encode_instruction(uint8_t *out, Instruction *inst, Program *prog, size_t pos, size_t *offsets) {
    if (!inst) return 0;
    char opc[32] = {0}; base_opcode(inst->opcode, opc);

    if (inst->noperands == 2) {
        Operand *dst = inst->operands[1];
        Operand *src = inst->operands[0];


        if (strcmp(opc, "mov") == 0) {
            if (src->kind == OP_REG && dst->kind == OP_REG) return encode_mov_reg_reg(out, dst, src);
            if (src->kind == OP_IMM && dst->kind == OP_REG) return encode_mov_imm_reg(out, dst, src);
            if (dst->kind == OP_MEM && src->kind == OP_REG) return encode_mov_reg_mem(out, dst, src);
            if (dst->kind == OP_REG && src->kind == OP_MEM) return encode_mov_mem_reg(out, dst, src);
            if (dst->kind == OP_MEM && src->kind == OP_IMM) return encode_mov_mem_imm(out, dst, src);
        }
        if (strcmp(opc, "add") == 0) {
            if (src->kind == OP_REG && dst->kind == OP_REG) return encode_add_reg_reg(out, dst, src);
            // if (src->kind == OP_IMM && dst->kind == OP_REG) return encode_add_imm_reg(out, dst, src);
            // if (src->kind == OP_IMM && dst->kind == OP_MEM) return encode_add_imm_mem(out, dst, src);
            // if (src->kind == OP_REG && dst->kind == OP_MEM) return encode_add_reg_mem(out, dst, src);
        // }
        // if (strcmp(opc, "sub") == 0) {
            // if (src->kind == OP_REG && dst->kind == OP_REG) return encode_sub_reg_reg(out, dst, src);
            // if (src->kind == OP_IMM && dst->kind == OP_REG) return encode_sub_imm_reg(out, dst, src);
            // if (src->kind == OP_IMM && dst->kind == OP_MEM) return encode_sub_imm_mem(out, dst, src);
            // if (src->kind == OP_REG && dst->kind == OP_MEM) return encode_sub_reg_mem(out, dst, src);
        // }
        // if (strcmp(opc, "cmp") == 0) {
            // if (src->kind == OP_REG && dst->kind == OP_REG) return encode_cmp_reg_reg(out, dst, src);
            // if (src->kind == OP_IMM && dst->kind == OP_REG) return encode_cmp_imm_reg(out, dst, src);
            // if (src->kind == OP_IMM && dst->kind == OP_MEM) return encode_cmp_imm_mem(out, dst, src);
            // if (src->kind == OP_REG && dst->kind == OP_MEM) return encode_cmp_reg_mem(out, dst, src);
        }
    }

    // if (strncmp(opc, "jmp", 3) == 0 && inst->noperands == 1)
        // return encode_jmp(out, inst->operands[0], prog, pos, offsets);

    fprintf(stderr, "Unsupported: %s\n", opc);
    return 0;
}

// -------------------- Compute instruction offsets --------------------
static size_t *compute_offsets(Program *prog) {
    if (!prog) return NULL;

    size_t *offsets = calloc(prog->nnodes, sizeof(size_t));
    if (!offsets) { perror("calloc"); exit(1); }

    size_t off = 0;
    for (size_t i = 0; i < prog->nnodes; i++) {
        offsets[i] = off;
        Node *n = prog->nodes[i];
        if (n->kind == NODE_INSTRUCTION) {
            // Rough estimate: instruction length. For jumps, assume 5 bytes (jmp rel32)
            char opc[32];
            base_opcode(n->u.instruction.opcode, opc);
            if (strncmp(opc, "jmp", 3) == 0) off += 5;
            else off += 5; // rough estimate for other instructions (mov/add/sub/cmp)
        }
    }
    return offsets;
}

// -------------------- Assemble program --------------------
void assemble_program(Program *prog,FILE *outf){
    if(!prog) return;
    size_t *offsets = compute_offsets(prog);
    uint8_t buf[32] = {0};
    for(size_t i=0;i<prog->nnodes;i++){
        Node *n=prog->nodes[i];
        if(n->kind!=NODE_INSTRUCTION) continue;
        size_t len=encode_instruction(buf,&n->u.instruction,prog,i,offsets);
        if(len==0){ fprintf(stderr,"Skipping unsupported: %s\n",n->u.instruction.opcode); continue;}
        fwrite(buf,1,len,outf);
    }
    free(offsets);
}