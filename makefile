CC = gcc

CFLAGS := -Wall -Wextra -g -Iinclude -Og -std=c11

C_SRC = $(shell find -type f -name '*.c')

OBJ = $(C_SRC:.c=.c.o)

jasm: $(OBJ)
	gcc $^ -o $@ $(CFLAGS) -Iinclude -lelf

%.c.o: %.c
	$(CC) -c $^ -o $@ $(CFLAGS)

run: jasm
	./$< test.s -o test.o -f elf

.PHONY: clean run
clean:
	find -type f -name '*.o' -delete
	rm jemu &> /dev/null || /bin/true
	rm -f test

test: run
	ld -o test test.o
	objdump -D test -m i386:x86-64

run_test: