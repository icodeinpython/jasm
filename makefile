CC = gcc

CFLAGS := -Wall -Wextra -g -Iinclude -Og

C_SRC = $(shell find -type f -name '*.c')

OBJ = $(C_SRC:.c=.c.o)

jasm: $(OBJ)
	gcc $^ -o $@ $(CFLAGS) -Iinclude

%.c.o: %.c
	$(CC) -c $^ -o $@ $(CFLAGS)

run: jasm
	./$< test.s test

.PHONY: clean run
clean:
	find -type f -name '*.o' -delete
	rm jemu &> /dev/null || /bin/true
	rm -f test

test: run
	objdump -D test -b binary -m i386:x86-64

run_test: