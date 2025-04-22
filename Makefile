

compile:
	gcc -Wall -g3 -fsanitize=address -pthread server.c -o server -lsqlite3
	gcc -Wall -g3 -fsanitize=address -pthread client.c -o client
FLAGS    = -L /lib64
LIBS     = -lusb-1.0 -l pthread


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
