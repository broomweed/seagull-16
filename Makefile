CC=gcc
CFLAGS=-Wall -g
LIBS=-lSDL2

test: main.o
	$(CC) $(CFLAGS) main.c $(LIBS) -o test

%.o.c: %.c %.h
	$(C) $(CFLAGS) $(LIBS) -c -o $@ $<

clean:
	rm *.o test
