#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include "jasm.h"


void parse_args(int argc, char* argv[], args_t* args) {
    if (argc < 3) {
        printf("Usage: %s <input> -o <output> [-f <binary|elf>]\n", argv[0]);
        exit(1);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            char *fmt = argv[++i];

            if (strcmp(fmt, "bin") == 0) {
                args->outformat = OF_BINARY;
            } else if (strcmp(fmt, "elf") == 0) {
                args->outformat = OF_ELF;
            } else {
                printf("Invalid output format: %s\n", fmt);
                exit(1);
            }
            continue;
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s <input> -o <output> [-f <binary|elf>]\n", argv[0]);
            exit(1);
        } else if (strcmp(argv[i], "-o") == 0) {
            args->outname = argv[i + 1];
            i++;
        } else {
            args->inname = argv[i];
        }
    }

}

FILE* infile = NULL;

args_t* args;

int main(int argc, char *argv[]) {
    args = malloc(sizeof(args_t));
    parse_args(argc, argv, args);

    infile = fopen(args->inname, "r");
    if (!infile) { perror("fopen"); exit(1); }

    Program* prog = parse_program(infile);
    dump_program(prog);
    fclose(infile);


    struct asm_ret* code = assemble_program(prog);


    if (args->outformat == OF_BINARY) {
        printf("Writing to %s\n", args->outname);
        int fd = open(args->outname, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open");
            exit(1);
        }
        write(fd, code->code, code->code_size);
        close(fd);
        return 0;
    }


    extern LabelTable* G_labels;

    for (size_t i = 0; i < G_labels->count; i++) {
        printf("%s: %#x\n", G_labels->entries[i].name, G_labels->entries[i].address);
    }

    for (size_t i = 0; i < G_relocs.count; i++) {
        printf("%s: %#x\n", G_relocs.entries[i].label->name, G_relocs.entries[i].offset);
    }

    write_elf64(args->outname, code,  G_labels, &G_relocs);



    return 0;
}