#pragma once

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

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

typedef struct Node {
    NodeType kind;

    union {
        char* label;
        struct {
            char* name;
            char** args;
            size_t nargs;
        } directive;
        Instruction instruction;
    } u;
} Node;

typedef struct {
    Node** nodes;
    size_t nnodes;
} Program;

Program* parse_program(FILE* f);
void dump_program(Program* p);
void assemble_program(Program* p, FILE* f);

typedef struct {
    char *name;
    uint32_t address;
} Label;

typedef struct {
    Label *entries;
    size_t count;
    size_t capacity;
} LabelTable;

size_t encode_instruction(uint8_t *out, Instruction *inst, Program *prog, size_t pos, LabelTable *label_table);