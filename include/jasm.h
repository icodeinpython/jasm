#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum {
    OF_BINARY,
    OF_ELF,
} OutFormat;

typedef struct {
    char *inname;
    char *outname;
    OutFormat outformat;
} args_t;

extern FILE* infile;

typedef enum {
    NODE_LABEL,
    NODE_DIRECTIVE,
    NODE_INSTRUCTION,
} NodeType;

typedef enum {
    OP_REG,
    OP_IMM,
    OP_MEM,
    OP_LABELREF,
} OperandType;

typedef struct {
    char* base;
    char* index;
    int scale;
    long disp;
    bool has_disp;
    int size;
} MemOperand;

typedef struct {
    OperandType kind;
    char* text;
    char* reg;
    long imm;
    char* labelref;
    MemOperand mem;
} Operand;

typedef struct {
    char* opcode;
    Operand** operands;
    size_t noperands;
    char* commment;
} Instruction;

typedef struct {
    char* name;
    char** args;
    size_t nargs;
} Directive;

typedef struct Node {
    NodeType kind;

    union {
        char* label;
        Directive directive;
        Instruction instruction;
    } u;
} Node;

typedef struct {
    Node** nodes;
    size_t nnodes;
} Program;

Program* parse_program(FILE* f);
void dump_program(Program* p);

typedef enum {
    SECTION_CODE,
    SECTION_DATA,
} Section;

typedef struct {
    char *name;
    uint32_t address;
    Section section;
} Label;

typedef struct {
    Label *entries;
    size_t count;
    size_t capacity;
} LabelTable;

typedef struct {
    Label *label;        // The label being referenced
    uint32_t offset;     // Where in the section the relocation occurs
    Section section;     // Which section this relocation belongs to
} Reloc;

typedef struct {
    Reloc *entries;
    size_t count;
    size_t capacity;
} RelocTable;


struct asm_ret {
    uint8_t* code;
    size_t code_size;
    uint8_t* data;
    size_t data_size;
};


struct asm_ret* assemble_program(Program *prog);
size_t encode_instruction(uint8_t *out, Instruction *inst, Program *prog, size_t pos, LabelTable *label_table);
int write_elf64(const char *filename, struct asm_ret *asmres, LabelTable *labels, RelocTable *relocs);

extern LabelTable* G_labels;
extern args_t* args;
extern RelocTable G_relocs;

void init_reloc_table(RelocTable *tbl);
void emit_reloc(RelocTable *tbl, Label* label, uint32_t offset, Section section);