CC=gcc
CFLAGS=-O2

kit: kit.c
	$(CC) $(CFLAGS) kit.c -o kit

clean:
	rm -f kit
