# Last edited on: 31-12-2024
# This is an example Makefile for the GrowingAndShrinkingBall example.

# Directory variables that we will use to compile the project.
PROJECTV_DIRECTORY = ../../../# This is the directory where the projectV library is located. (relative to this file in this case)
INCLUDE_DIR = $(PROJECTV_DIRECTORY)include/# This is the directory where the projectV headers are located.
LIB = $(PROJECTV_DIRECTORY)lib/# This is the directory where the projectV libraries are located.

# Compiler and flags. This example uses all 3 modules, so all must be linked. Along with GLEW, GLFW and OpenGL
CXX = g++# Using g++ compiler
CXXFLAGS = -I$(INCLUDE_DIR) -O3# -I specifies the include directory as $(INCLUDE_DIR). The -O3 flag is used to optimize the code.
LDFLAGS = -L$(LIB) -lprojectV-utils -lprojectV-graphics -lprojectV-core -lGLEW -lglfw -lGL# -L specifies the library directory as $(LIB). The -l flags are used to link the libraries.

# Target
all: main.o # The file that we want to compile

main.o: main.cpp# A rule that specifies how to compile main.cpp into main.o. Look closely, it is just a normal compilation command.
	$(CXX) $(CXXFLAGS) main.cpp -o main.o $(LDFLAGS)

# Cleans the directory from the compiled files.
.PHONY: all clean

