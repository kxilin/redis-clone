# Compiler and compilation flags
CXX = g++
CXXFLAGS = -Wall -Wextra -O2
LDFLAGS = 

# Target executable names
SERVER = server
CLIENT = client

# Source files for each component
UTILS_SRC = utils.cpp
HASHTABLE_SRC = hashtable.cpp
SERVER_SRC = server.cpp
CLIENT_SRC = client.cpp

# Object files generated from source file names
UTILS_OBJ = $(UTILS_SRC:.cpp=.o)
HASHTABLE_OBJ = $(HASHTABLE_SRC:.cpp=.o)
SERVER_OBJ = $(SERVER_SRC:.cpp=.o)
CLIENT_OBJ = $(CLIENT_SRC:.cpp=.o)

# Default rule to build both server and client
all: $(SERVER) $(CLIENT)

# Linking rule for the server executable
$(SERVER): $(SERVER_OBJ) $(UTILS_OBJ) $(HASHTABLE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Linking rule for the client executable
$(CLIENT): $(CLIENT_OBJ) $(UTILS_OBJ) $(HASHTABLE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Pattern rule to compile .cpp files into .o files
# Rebuilds if utils.h or hashtable.h are modified
%.o: %.cpp utils.h hashtable.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to remove generated build files
clean:
	rm -f $(SERVER) $(CLIENT) *.o

# Declare targets that do not represent actual files
.PHONY: all clean
