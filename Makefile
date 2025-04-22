# Compiler to use
CC = gcc

# Compiler flags
CFLAGS = -Wall -g3 -fsanitize=address -pthread

# Library flags
FLAGS = -L /lib64
LIBS = -lsqlite3

# Targets
all: server client

server: server.c
	$(CC) $(CFLAGS) server.c -o server $(FLAGS) $(LIBS)

client: client.c
	$(CC) $(CFLAGS) client.c -o client

clean:
	rm -f server client

.PHONY: all clean
