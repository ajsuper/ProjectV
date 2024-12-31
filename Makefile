# Last edited on: 31-12-2024
# Compiler and flags
CXX = g++
CXXFLAGS = -I./include -Wall -Wextra -g -O3

# Directories
SRC_DIR = ./src
OBJ_DIR = ./obj
LIB_DIR = ./lib
BUILD_DIR = ./build
INCLUDE_DIR = ./include

# Source files
SOURCES = $(SRC_DIR)/core.cpp $(SRC_DIR)/graphics.cpp $(SRC_DIR)/utils.cpp

# Object files
OBJECTS = $(OBJ_DIR)/core.o $(OBJ_DIR)/graphics.o $(OBJ_DIR)/utils.o

# Static Libraries
LIBS = $(LIB_DIR)/libprojectV-core.a $(LIB_DIR)/libprojectV-graphics.a $(LIB_DIR)/libprojectV-utils.a

# Targets
all: $(LIBS)

# Rule to create .a (static libraries)
$(LIB_DIR)/libprojectV-core.a: $(OBJ_DIR)/core.o | $(LIB_DIR)
	@echo "Creating $@"
	ar rcs $@ $<

$(LIB_DIR)/libprojectV-graphics.a: $(OBJ_DIR)/graphics.o | $(LIB_DIR)
	@echo "Creating $@"
	ar rcs $@ $<

$(LIB_DIR)/libprojectV-utils.a: $(OBJ_DIR)/utils.o | $(LIB_DIR)
	@echo "Creating $@"
	ar rcs $@ $<

# Rule to compile .cpp files into .o files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	@echo "Compiling $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Create necessary directories if they don't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(LIB_DIR):
	mkdir -p $(LIB_DIR)

# Clean up object files and libraries
clean:
	@echo "Cleaning up..."
	rm -rf $(OBJ_DIR)/*.o $(LIBS)

# Phony targets (not actual files)
.PHONY: all clean

