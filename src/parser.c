#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "jasm.h"


/* ---------- Helpers ---------- */

static char *strdup_safe(const char *s) {
    if (!s) return NULL;
    char *r = malloc(strlen(s) + 1);
    if (!r) { perror("malloc"); exit(1); }
    strcpy(r, s);
    return r;
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { perror("malloc"); exit(1); }
    return p;
}

/* ---------- Tokenizer ---------- */

typedef enum {
    T_EOF, T_IDENT, T_DIRECTIVE, T_REGISTER, T_NUMBER, T_IMM_PREFIX,
    T_LPAREN, T_RPAREN, T_COMMA, T_COLON, T_NEWLINE, T_COMMENT, T_OTHER
} TokenType;

typedef struct {
    TokenType type;
    char *text;
} Token;

typedef struct {
    Token *toks;
    size_t len, cap, i;
} TokenStream;

static void add_tok(TokenStream *ts, TokenType type, const char *s, size_t n) {
    if (ts->len == ts->cap) {
        ts->cap = ts->cap ? ts->cap * 2 : 128;
        ts->toks = realloc(ts->toks, ts->cap * sizeof(Token));
        if (!ts->toks) { perror("realloc"); exit(1); }
    }
    ts->toks[ts->len].type = type;
    ts->toks[ts->len].text = strndup(s, n);
    ts->len++;
}

static void lex_file(TokenStream *ts, FILE *f) {
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t i = 0;
        while (line[i]) {
            char c = line[i];
            if (c == '#' || c == ';' || (c == '/' && line[i+1] == '/')) {
                add_tok(ts, T_COMMENT, line + i, strlen(line + i));
                break;
            } else if (isspace((unsigned char)c)) {
                if (c == '\n') add_tok(ts, T_NEWLINE, &c, 1);
                i++;
            } else if (c == '%') {
                size_t start = i++;
                while (isalnum((unsigned char)line[i])) i++;
                add_tok(ts, T_REGISTER, line + start, i - start);
            } else if (c == '$') {
                add_tok(ts, T_IMM_PREFIX, &c, 1);
                i++;
            } else if (c == '.') {
                size_t start = i++;
                while (isalnum((unsigned char)line[i]) || line[i] == '_') i++;
                add_tok(ts, T_DIRECTIVE, line + start, i - start);
            } else if (isdigit((unsigned char)c)) {
                size_t start = i++;
                while (isalnum((unsigned char)line[i]) || line[i]=='x' || line[i]=='X') i++;
                add_tok(ts, T_NUMBER, line + start, i - start);
            } else if (isalpha((unsigned char)c) || c == '_' || c == '$') {
                size_t start = i++;
                while (isalnum((unsigned char)line[i]) || line[i]=='_' || line[i]=='$') i++;
                if (line[i] == ':') {
                    add_tok(ts, T_IDENT, line + start, i - start);
                    add_tok(ts, T_COLON, ":", 1);
                    i++;
                } else {
                    add_tok(ts, T_IDENT, line + start, i - start);
                }
            } else if (c == '(') { add_tok(ts, T_LPAREN, &c, 1); i++; }
            else if (c == ')') { add_tok(ts, T_RPAREN, &c, 1); i++; }
            else if (c == ',') { add_tok(ts, T_COMMA, &c, 1); i++; }
            else { add_tok(ts, T_OTHER, &c, 1); i++; }
        }
    }
    add_tok(ts, T_EOF, "", 0);
}

/* ---------- Parser ---------- */

static Token *peek(TokenStream *ts) {
    return &ts->toks[ts->i];
}

static Token *next(TokenStream *ts) {
    return &ts->toks[ts->i++];
}

static bool accept(TokenStream *ts, TokenType t) {
    if (peek(ts)->type == t) { ts->i++; return true; }
    return false;
}

static long parse_number(const char *s) {
    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0)
        return strtol(s + 2, NULL, 16);
    return strtol(s, NULL, 10);
}

/* Operand parsing */
static Operand *parse_operand(TokenStream *ts);

static Operand *parse_mem(TokenStream *ts, long disp, bool has_disp) {
    Operand *op = xmalloc(sizeof(Operand));
    memset(op, 0, sizeof(Operand));
    op->kind = OP_MEM;
    op->mem.scale = 1;
    op->mem.disp = disp;
    op->mem.has_disp = has_disp;

    accept(ts, T_LPAREN);
    if (peek(ts)->type == T_REGISTER) {
        op->mem.base = strdup_safe(peek(ts)->text);
        next(ts);
    }
    if (accept(ts, T_COMMA)) {
        if (peek(ts)->type == T_REGISTER) {
            op->mem.index = strdup_safe(peek(ts)->text);
            next(ts);
        }
        if (accept(ts, T_COMMA)) {
            if (peek(ts)->type == T_NUMBER) {
                op->mem.scale = atoi(peek(ts)->text);
                next(ts);
            }
        }
    }
    accept(ts, T_RPAREN);
    return op;
}

static Operand *parse_operand(TokenStream *ts) {
    Token *t = peek(ts);
    Operand *op = xmalloc(sizeof(Operand));
    memset(op, 0, sizeof(Operand));

    if (t->type == T_IMM_PREFIX) {
        next(ts);
        Token *n = next(ts);
        op->kind = OP_IMM;
        op->imm = parse_number(n->text);
        return op;
    }
    if (t->type == T_REGISTER) {
        op->kind = OP_REG;
        op->reg = strdup_safe(t->text);
        next(ts);
        return op;
    }
    if (t->type == T_NUMBER) {
        long disp = parse_number(t->text);
        next(ts);
        if (peek(ts)->type == T_LPAREN)
            return parse_mem(ts, disp, true);
        op->kind = OP_IMM;
        op->imm = disp;
        return op;
    }
    if (t->type == T_LPAREN)
        return parse_mem(ts, 0, false);
    if (t->type == T_IDENT) {
        op->kind = OP_LABELREF;
        op->labelref = strdup_safe(t->text);
        next(ts);
        return op;
    }
    next(ts);
    return op;
}

/* Instruction, directive, label parsing */

static Node *parse_instruction(TokenStream *ts, const char *opcode) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = NODE_INSTRUCTION;
    n->u.instruction.opcode = strdup_safe(opcode);

    n->u.instruction.operands = NULL;
    n->u.instruction.noperands = 0;

    while (!accept(ts, T_NEWLINE) && peek(ts)->type != T_COMMENT && peek(ts)->type != T_EOF) {
        Operand *op = parse_operand(ts);
        n->u.instruction.operands = realloc(
            n->u.instruction.operands,
            (n->u.instruction.noperands + 1) * sizeof(Operand *)
        );
        n->u.instruction.operands[n->u.instruction.noperands++] = op;
        accept(ts, T_COMMA);
    }
    return n;
}

static Node *parse_directive(TokenStream *ts, const char *name) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = NODE_DIRECTIVE;
    n->u.directive.name = strdup_safe(name);
    n->u.directive.args = NULL;
    n->u.directive.nargs = 0;

    while (!accept(ts, T_NEWLINE) && peek(ts)->type != T_COMMENT && peek(ts)->type != T_EOF) {
        if (peek(ts)->type == T_IDENT || peek(ts)->type == T_NUMBER) {
            n->u.directive.args = realloc(
                n->u.directive.args,
                (n->u.directive.nargs + 1) * sizeof(char *)
            );
            n->u.directive.args[n->u.directive.nargs++] = strdup_safe(peek(ts)->text);
            next(ts);
        } else next(ts);
    }
    return n;
}

static Node *parse_label(const char *name) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = NODE_LABEL;
    n->u.label = strdup_safe(name);
    return n;
}

/* Main program parse */
Program *parse_program(FILE *f) {
    TokenStream ts = {0};
    lex_file(&ts, f);

    Program *prog = xmalloc(sizeof(Program));
    prog->nodes = NULL;
    prog->nnodes = 0;

    while (peek(&ts)->type != T_EOF) {
        Token *t = peek(&ts);
        if (t->type == T_COMMENT || t->type == T_NEWLINE) { next(&ts); continue; }

        Node *node = NULL;
        if (t->type == T_IDENT && ts.toks[ts.i+1].type == T_COLON) {
            node = parse_label(t->text);
            ts.i += 2; // skip ident + colon
        } else if (t->type == T_DIRECTIVE) {
            node = parse_directive(&ts, t->text);
            next(&ts);
        } else if (t->type == T_IDENT) {
            next(&ts);
            node = parse_instruction(&ts, t->text);
        } else { next(&ts); }

        if (node) {
            prog->nodes = realloc(prog->nodes, (prog->nnodes + 1) * sizeof(Node *));
            prog->nodes[prog->nnodes++] = node;
        }
    }
    return prog;
}

/* ---------- Pretty printer ---------- */
void dump_program(Program *p) {
    for (size_t i = 0; i < p->nnodes; i++) {
        Node *n = p->nodes[i];
        switch (n->kind) {
        case NODE_LABEL:
            printf("Label: %s\n", n->u.label);
            break;
        case NODE_DIRECTIVE:
            printf("Directive: %s", n->u.directive.name);
            for (size_t j = 0; j < n->u.directive.nargs; j++)
                printf(" %s", n->u.directive.args[j]);
            printf("\n");
            break;
        case NODE_INSTRUCTION:
            printf("Instr: %s", n->u.instruction.opcode);
            for (size_t j = 0; j < n->u.instruction.noperands; j++) {
                Operand *o = n->u.instruction.operands[j];
                printf(" ");
                switch (o->kind) {
                case OP_REG: printf("%s", o->reg); break;
                case OP_IMM: printf("$%ld", o->imm); break;
                case OP_LABELREF: printf("%s", o->labelref); break;
                case OP_MEM:
                    printf("%ld(%s,%s,%d)", o->mem.disp,
                        o->mem.base ? o->mem.base : "",
                        o->mem.index ? o->mem.index : "",
                        o->mem.scale);
                    break;
                }
                if (j + 1 < n->u.instruction.noperands) printf(",");
            }
            printf("\n");
            break;
        }
    }
}