# Host-side tests for main/game.c and main/scores.c. Needs only a C compiler:
# `make -C test`. Run these before every flash.

CC      ?= cc
CFLAGS  := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
           -fno-sanitize-recover=all -I../main

TESTS := test_game test_scores

all: test

test_game: test_game.c ../main/game.c ../main/game.h
	$(CC) $(CFLAGS) -o $@ test_game.c ../main/game.c

test_scores: test_scores.c ../main/scores.c ../main/scores.h
	$(CC) $(CFLAGS) -o $@ test_scores.c ../main/scores.c

.PHONY: all test clean
test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done

clean:
	rm -f $(TESTS)
