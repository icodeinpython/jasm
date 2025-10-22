#include <stdio.h>
#include <stdlib.h>
#include "jasm.h"

typedef struct {
    char *inname;
    char *outname;
} args_t;

void parse_args(int argc, char* argv[], args_t* args) {
    if (argc != 3) {
        printf("Usage: %s <input> <output>\n", argv[0]);
        exit(1);
    }
    args->inname = argv[1];
    args->outname = argv[2];
}

FILE* infile = NULL;

int main(int argc, char *argv[]) {
    args_t args;
    parse_args(argc, argv, &args);

    infile = fopen(args.inname, "r");
    if (!infile) { perror("fopen"); exit(1); }

    Program* prog = parse_program(infile);
    dump_program(prog);
    fclose(infile);

    FILE* outfile = fopen(args.outname, "wb");
    assemble_program(prog, outfile);
    fclose(outfile);

    return 0;
}