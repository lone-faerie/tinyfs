CC= gcc
CFLAGS= -g -Wall -std=gnu11

OBJS = libDisk.o libTinyFS.o slice.o bitset.o

all: diskTest tfsTest

debug: CFLAGS += -DDEBUG_FLAG
debug: diskTest tfsTest

diskTest: $(OBJS)
	$(CC) $(CFLAGS) -o diskTest diskTest.c $(OBJS)

tfsTest: $(OBJS)
	$(CC) $(CFLAGS) -o tfsTest tfsTest.c $(OBJS)

.c.o:
	gcc -c $(CFLAGS) $< -o $@

clean:
	rm -f diskTest tfsTest *.o
