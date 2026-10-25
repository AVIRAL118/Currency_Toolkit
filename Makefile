CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -pedantic
LDLIBS  ?= -lm

engine: currency_engine.c
	$(CC) $(CFLAGS) -o engine_test currency_engine.c $(LDLIBS)

clean:
	rm -f engine_test engine_test.exe

.PHONY: engine clean