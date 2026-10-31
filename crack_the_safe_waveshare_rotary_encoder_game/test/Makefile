# Host-side tests for main/game.c. Needs only a C compiler: `make -C test`.
# Run these before every flash.

CC      ?= cc
CFLAGS  := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
           -fno-sanitize-recover=all -I../main

test_game: test_game.c ../main/game.c ../main/game.h
	$(CC) $(CFLAGS) -o $@ test_game.c ../main/game.c

.PHONY: test clean
test: test_game
	./test_game

clean:
	rm -f test_game
