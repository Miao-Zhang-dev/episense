

OBJ=web
SRC=main.c net.c db.c
CC=gcc
INC=../include
CFLAGS=-std=c11 -O2 -Wall -Wextra
LDFLAGS=-lsqlite3 -lcrypto -lm

$(OBJ):$(SRC)
	$(CC) $(CFLAGS) $^ -o $@ -I$(INC) $(LDFLAGS)

benchmark: benchmark.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(OBJ) benchmark
