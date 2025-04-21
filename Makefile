# Compiler to use
CC = gcc

# Compiler flags
CFLAGS = -Wall -g3 -fsanitize=address -pthread

# Linker flags (libraries)
LDFLAGS = -lsqlite3 -fsanitize=address -pthread

# Source files for server
SERVER_SOURCES = server.c client_manager.c database.c room.c

# Source files for client
CLIENT_SOURCES = client.c client_manager.c

# Object files (generated from source files)
SERVER_OBJECTS = $(SERVER_SOURCES:.c=.o)
CLIENT_OBJECTS = $(CLIENT_SOURCES:.c=.o)

# Executable names
SERVER_EXEC = server
CLIENT_EXEC = client

# Default target
all: $(SERVER_EXEC) $(CLIENT_EXEC)

# Link object files to create the server executable
$(SERVER_EXEC): $(SERVER_OBJECTS)
	$(CC) $(SERVER_OBJECTS) -o $@ $(LDFLAGS)

# Link object files to create the client executable
$(CLIENT_EXEC): $(CLIENT_OBJECTS)
	$(CC) $(CLIENT_OBJECTS) -o $@ $(LDFLAGS)

# Compile source files to object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up
clean:
	rm -f $(SERVER_OBJECTS) $(CLIENT_OBJECTS) $(SERVER_EXEC) $(CLIENT_EXEC)

# Phony targets
.PHONY: all clean
