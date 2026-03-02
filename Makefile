# Compiler and compilation flags
CXX = g++
CXXFLAGS = -Wall -Wextra -O2
LDFLAGS = 

# Target executable names
SERVER = server
CLIENT = client
TEST_OFFSET = test_offset
TEST_AVL = test_avl

# Source files for each component
UTILS_SRC = utils.cpp
HASHTABLE_SRC = hashtable.cpp
AVL_SRC = avl.cpp
ZSET_SRC = zset.cpp
HEAP_SRC = heap.cpp
SERVER_SRC = server.cpp
CLIENT_SRC = client.cpp
TEST_OFFSET_SRC = test_offset.cpp

# Object files generated from source file names
UTILS_OBJ = $(UTILS_SRC:.cpp=.o)
HASHTABLE_OBJ = $(HASHTABLE_SRC:.cpp=.o)
AVL_OBJ = $(AVL_SRC:.cpp=.o)
ZSET_OBJ = $(ZSET_SRC:.cpp=.o)
HEAP_OBJ = $(HEAP_SRC:.cpp=.o)
SERVER_OBJ = $(SERVER_SRC:.cpp=.o)
CLIENT_OBJ = $(CLIENT_SRC:.cpp=.o)
TEST_OFFSET_OBJ = $(TEST_OFFSET_SRC:.cpp=.o)

# Default rule to build both server and client
all: $(SERVER) $(CLIENT) $(TEST_OFFSET)

# Linking rule for the server executable
$(SERVER): $(SERVER_OBJ) $(UTILS_OBJ) $(HASHTABLE_OBJ) $(AVL_OBJ) $(ZSET_OBJ) $(HEAP_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Linking rule for the client executable
$(CLIENT): $(CLIENT_OBJ) $(UTILS_OBJ) $(HASHTABLE_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Linking rule for test_offset
$(TEST_OFFSET): $(TEST_OFFSET_OBJ) $(AVL_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Pattern rule to compile .cpp files into .o files
%.o: %.cpp utils.h hashtable.h avl.h zset.h heap.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Rule to remove generated build files
clean:
	rm -f $(SERVER) $(CLIENT) $(TEST_AVL) $(TEST_OFFSET) *.o

# Declare targets that do not represent actual files
.PHONY: all clean
