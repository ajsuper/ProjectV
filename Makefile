# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.28

# Default target executed when no arguments are given to make.
default_target: all
.PHONY : default_target

# Allow only one "make -f Makefile2" at a time, but pass parallelism.
.NOTPARALLEL:

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Disable VCS-based implicit rules.
% : %,v

# Disable VCS-based implicit rules.
% : RCS/%

# Disable VCS-based implicit rules.
% : RCS/%,v

# Disable VCS-based implicit rules.
% : SCCS/s.%

# Disable VCS-based implicit rules.
% : s.%

.SUFFIXES: .hpux_make_needs_suffix_list

# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/bin/cmake

# The command to remove a file.
RM = /usr/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/andrew/Development/CPP/ProjectV

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/andrew/Development/CPP/ProjectV

#=============================================================================
# Targets provided globally by CMake.

# Special rule for the target edit_cache
edit_cache:
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --cyan "No interactive CMake dialog available..."
	/usr/bin/cmake -E echo No\ interactive\ CMake\ dialog\ available.
.PHONY : edit_cache

# Special rule for the target edit_cache
edit_cache/fast: edit_cache
.PHONY : edit_cache/fast

# Special rule for the target rebuild_cache
rebuild_cache:
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --cyan "Running CMake to regenerate build system..."
	/usr/bin/cmake --regenerate-during-build -S$(CMAKE_SOURCE_DIR) -B$(CMAKE_BINARY_DIR)
.PHONY : rebuild_cache

# Special rule for the target rebuild_cache
rebuild_cache/fast: rebuild_cache
.PHONY : rebuild_cache/fast

# The main all target
all: cmake_check_build_system
	$(CMAKE_COMMAND) -E cmake_progress_start /home/andrew/Development/CPP/ProjectV/CMakeFiles /home/andrew/Development/CPP/ProjectV//CMakeFiles/progress.marks
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 all
	$(CMAKE_COMMAND) -E cmake_progress_start /home/andrew/Development/CPP/ProjectV/CMakeFiles 0
.PHONY : all

# The main clean target
clean:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 clean
.PHONY : clean

# The main clean target
clean/fast: clean
.PHONY : clean/fast

# Prepare targets for installation.
preinstall: all
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 preinstall
.PHONY : preinstall

# Prepare targets for installation.
preinstall/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 preinstall
.PHONY : preinstall/fast

# clear depends
depend:
	$(CMAKE_COMMAND) -S$(CMAKE_SOURCE_DIR) -B$(CMAKE_BINARY_DIR) --check-build-system CMakeFiles/Makefile.cmake 1
.PHONY : depend

#=============================================================================
# Target rules for targets named projectV-ecs

# Build rule for target.
projectV-ecs: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-ecs
.PHONY : projectV-ecs

# fast build rule for target.
projectV-ecs/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-ecs.dir/build.make CMakeFiles/projectV-ecs.dir/build
.PHONY : projectV-ecs/fast

#=============================================================================
# Target rules for targets named projectV-fbo

# Build rule for target.
projectV-fbo: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-fbo
.PHONY : projectV-fbo

# fast build rule for target.
projectV-fbo/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-fbo.dir/build.make CMakeFiles/projectV-fbo.dir/build
.PHONY : projectV-fbo/fast

#=============================================================================
# Target rules for targets named projectV-render

# Build rule for target.
projectV-render: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-render
.PHONY : projectV-render

# fast build rule for target.
projectV-render/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-render.dir/build.make CMakeFiles/projectV-render.dir/build
.PHONY : projectV-render/fast

#=============================================================================
# Target rules for targets named projectV-scene

# Build rule for target.
projectV-scene: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-scene
.PHONY : projectV-scene

# fast build rule for target.
projectV-scene/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-scene.dir/build.make CMakeFiles/projectV-scene.dir/build
.PHONY : projectV-scene/fast

#=============================================================================
# Target rules for targets named projectV-shader

# Build rule for target.
projectV-shader: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-shader
.PHONY : projectV-shader

# fast build rule for target.
projectV-shader/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-shader.dir/build.make CMakeFiles/projectV-shader.dir/build
.PHONY : projectV-shader/fast

#=============================================================================
# Target rules for targets named projectV-uniforms

# Build rule for target.
projectV-uniforms: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-uniforms
.PHONY : projectV-uniforms

# fast build rule for target.
projectV-uniforms/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-uniforms.dir/build.make CMakeFiles/projectV-uniforms.dir/build
.PHONY : projectV-uniforms/fast

#=============================================================================
# Target rules for targets named projectV-user_input

# Build rule for target.
projectV-user_input: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-user_input
.PHONY : projectV-user_input

# fast build rule for target.
projectV-user_input/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-user_input.dir/build.make CMakeFiles/projectV-user_input.dir/build
.PHONY : projectV-user_input/fast

#=============================================================================
# Target rules for targets named projectV-window

# Build rule for target.
projectV-window: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-window
.PHONY : projectV-window

# fast build rule for target.
projectV-window/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-window.dir/build.make CMakeFiles/projectV-window.dir/build
.PHONY : projectV-window/fast

#=============================================================================
# Target rules for targets named projectV-lod

# Build rule for target.
projectV-lod: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-lod
.PHONY : projectV-lod

# fast build rule for target.
projectV-lod/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-lod.dir/build.make CMakeFiles/projectV-lod.dir/build
.PHONY : projectV-lod/fast

#=============================================================================
# Target rules for targets named projectV-voxel_io

# Build rule for target.
projectV-voxel_io: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-voxel_io
.PHONY : projectV-voxel_io

# fast build rule for target.
projectV-voxel_io/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_io.dir/build.make CMakeFiles/projectV-voxel_io.dir/build
.PHONY : projectV-voxel_io/fast

#=============================================================================
# Target rules for targets named projectV-voxel_management

# Build rule for target.
projectV-voxel_management: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-voxel_management
.PHONY : projectV-voxel_management

# fast build rule for target.
projectV-voxel_management/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_management.dir/build.make CMakeFiles/projectV-voxel_management.dir/build
.PHONY : projectV-voxel_management/fast

#=============================================================================
# Target rules for targets named projectV-voxel_math

# Build rule for target.
projectV-voxel_math: cmake_check_build_system
	$(MAKE) $(MAKESILENT) -f CMakeFiles/Makefile2 projectV-voxel_math
.PHONY : projectV-voxel_math

# fast build rule for target.
projectV-voxel_math/fast:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_math.dir/build.make CMakeFiles/projectV-voxel_math.dir/build
.PHONY : projectV-voxel_math/fast

src/core/ecs.o: src/core/ecs.cpp.o
.PHONY : src/core/ecs.o

# target to build an object file
src/core/ecs.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-ecs.dir/build.make CMakeFiles/projectV-ecs.dir/src/core/ecs.cpp.o
.PHONY : src/core/ecs.cpp.o

src/core/ecs.i: src/core/ecs.cpp.i
.PHONY : src/core/ecs.i

# target to preprocess a source file
src/core/ecs.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-ecs.dir/build.make CMakeFiles/projectV-ecs.dir/src/core/ecs.cpp.i
.PHONY : src/core/ecs.cpp.i

src/core/ecs.s: src/core/ecs.cpp.s
.PHONY : src/core/ecs.s

# target to generate assembly for a file
src/core/ecs.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-ecs.dir/build.make CMakeFiles/projectV-ecs.dir/src/core/ecs.cpp.s
.PHONY : src/core/ecs.cpp.s

src/graphics/fbo.o: src/graphics/fbo.cpp.o
.PHONY : src/graphics/fbo.o

# target to build an object file
src/graphics/fbo.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-fbo.dir/build.make CMakeFiles/projectV-fbo.dir/src/graphics/fbo.cpp.o
.PHONY : src/graphics/fbo.cpp.o

src/graphics/fbo.i: src/graphics/fbo.cpp.i
.PHONY : src/graphics/fbo.i

# target to preprocess a source file
src/graphics/fbo.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-fbo.dir/build.make CMakeFiles/projectV-fbo.dir/src/graphics/fbo.cpp.i
.PHONY : src/graphics/fbo.cpp.i

src/graphics/fbo.s: src/graphics/fbo.cpp.s
.PHONY : src/graphics/fbo.s

# target to generate assembly for a file
src/graphics/fbo.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-fbo.dir/build.make CMakeFiles/projectV-fbo.dir/src/graphics/fbo.cpp.s
.PHONY : src/graphics/fbo.cpp.s

src/graphics/render.o: src/graphics/render.cpp.o
.PHONY : src/graphics/render.o

# target to build an object file
src/graphics/render.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-render.dir/build.make CMakeFiles/projectV-render.dir/src/graphics/render.cpp.o
.PHONY : src/graphics/render.cpp.o

src/graphics/render.i: src/graphics/render.cpp.i
.PHONY : src/graphics/render.i

# target to preprocess a source file
src/graphics/render.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-render.dir/build.make CMakeFiles/projectV-render.dir/src/graphics/render.cpp.i
.PHONY : src/graphics/render.cpp.i

src/graphics/render.s: src/graphics/render.cpp.s
.PHONY : src/graphics/render.s

# target to generate assembly for a file
src/graphics/render.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-render.dir/build.make CMakeFiles/projectV-render.dir/src/graphics/render.cpp.s
.PHONY : src/graphics/render.cpp.s

src/graphics/scene.o: src/graphics/scene.cpp.o
.PHONY : src/graphics/scene.o

# target to build an object file
src/graphics/scene.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-scene.dir/build.make CMakeFiles/projectV-scene.dir/src/graphics/scene.cpp.o
.PHONY : src/graphics/scene.cpp.o

src/graphics/scene.i: src/graphics/scene.cpp.i
.PHONY : src/graphics/scene.i

# target to preprocess a source file
src/graphics/scene.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-scene.dir/build.make CMakeFiles/projectV-scene.dir/src/graphics/scene.cpp.i
.PHONY : src/graphics/scene.cpp.i

src/graphics/scene.s: src/graphics/scene.cpp.s
.PHONY : src/graphics/scene.s

# target to generate assembly for a file
src/graphics/scene.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-scene.dir/build.make CMakeFiles/projectV-scene.dir/src/graphics/scene.cpp.s
.PHONY : src/graphics/scene.cpp.s

src/graphics/shader.o: src/graphics/shader.cpp.o
.PHONY : src/graphics/shader.o

# target to build an object file
src/graphics/shader.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-shader.dir/build.make CMakeFiles/projectV-shader.dir/src/graphics/shader.cpp.o
.PHONY : src/graphics/shader.cpp.o

src/graphics/shader.i: src/graphics/shader.cpp.i
.PHONY : src/graphics/shader.i

# target to preprocess a source file
src/graphics/shader.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-shader.dir/build.make CMakeFiles/projectV-shader.dir/src/graphics/shader.cpp.i
.PHONY : src/graphics/shader.cpp.i

src/graphics/shader.s: src/graphics/shader.cpp.s
.PHONY : src/graphics/shader.s

# target to generate assembly for a file
src/graphics/shader.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-shader.dir/build.make CMakeFiles/projectV-shader.dir/src/graphics/shader.cpp.s
.PHONY : src/graphics/shader.cpp.s

src/graphics/uniforms.o: src/graphics/uniforms.cpp.o
.PHONY : src/graphics/uniforms.o

# target to build an object file
src/graphics/uniforms.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-uniforms.dir/build.make CMakeFiles/projectV-uniforms.dir/src/graphics/uniforms.cpp.o
.PHONY : src/graphics/uniforms.cpp.o

src/graphics/uniforms.i: src/graphics/uniforms.cpp.i
.PHONY : src/graphics/uniforms.i

# target to preprocess a source file
src/graphics/uniforms.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-uniforms.dir/build.make CMakeFiles/projectV-uniforms.dir/src/graphics/uniforms.cpp.i
.PHONY : src/graphics/uniforms.cpp.i

src/graphics/uniforms.s: src/graphics/uniforms.cpp.s
.PHONY : src/graphics/uniforms.s

# target to generate assembly for a file
src/graphics/uniforms.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-uniforms.dir/build.make CMakeFiles/projectV-uniforms.dir/src/graphics/uniforms.cpp.s
.PHONY : src/graphics/uniforms.cpp.s

src/graphics/user_input.o: src/graphics/user_input.cpp.o
.PHONY : src/graphics/user_input.o

# target to build an object file
src/graphics/user_input.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-user_input.dir/build.make CMakeFiles/projectV-user_input.dir/src/graphics/user_input.cpp.o
.PHONY : src/graphics/user_input.cpp.o

src/graphics/user_input.i: src/graphics/user_input.cpp.i
.PHONY : src/graphics/user_input.i

# target to preprocess a source file
src/graphics/user_input.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-user_input.dir/build.make CMakeFiles/projectV-user_input.dir/src/graphics/user_input.cpp.i
.PHONY : src/graphics/user_input.cpp.i

src/graphics/user_input.s: src/graphics/user_input.cpp.s
.PHONY : src/graphics/user_input.s

# target to generate assembly for a file
src/graphics/user_input.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-user_input.dir/build.make CMakeFiles/projectV-user_input.dir/src/graphics/user_input.cpp.s
.PHONY : src/graphics/user_input.cpp.s

src/graphics/window.o: src/graphics/window.cpp.o
.PHONY : src/graphics/window.o

# target to build an object file
src/graphics/window.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-window.dir/build.make CMakeFiles/projectV-window.dir/src/graphics/window.cpp.o
.PHONY : src/graphics/window.cpp.o

src/graphics/window.i: src/graphics/window.cpp.i
.PHONY : src/graphics/window.i

# target to preprocess a source file
src/graphics/window.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-window.dir/build.make CMakeFiles/projectV-window.dir/src/graphics/window.cpp.i
.PHONY : src/graphics/window.cpp.i

src/graphics/window.s: src/graphics/window.cpp.s
.PHONY : src/graphics/window.s

# target to generate assembly for a file
src/graphics/window.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-window.dir/build.make CMakeFiles/projectV-window.dir/src/graphics/window.cpp.s
.PHONY : src/graphics/window.cpp.s

src/utils/lod.o: src/utils/lod.cpp.o
.PHONY : src/utils/lod.o

# target to build an object file
src/utils/lod.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-lod.dir/build.make CMakeFiles/projectV-lod.dir/src/utils/lod.cpp.o
.PHONY : src/utils/lod.cpp.o

src/utils/lod.i: src/utils/lod.cpp.i
.PHONY : src/utils/lod.i

# target to preprocess a source file
src/utils/lod.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-lod.dir/build.make CMakeFiles/projectV-lod.dir/src/utils/lod.cpp.i
.PHONY : src/utils/lod.cpp.i

src/utils/lod.s: src/utils/lod.cpp.s
.PHONY : src/utils/lod.s

# target to generate assembly for a file
src/utils/lod.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-lod.dir/build.make CMakeFiles/projectV-lod.dir/src/utils/lod.cpp.s
.PHONY : src/utils/lod.cpp.s

src/utils/voxel_io.o: src/utils/voxel_io.cpp.o
.PHONY : src/utils/voxel_io.o

# target to build an object file
src/utils/voxel_io.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_io.dir/build.make CMakeFiles/projectV-voxel_io.dir/src/utils/voxel_io.cpp.o
.PHONY : src/utils/voxel_io.cpp.o

src/utils/voxel_io.i: src/utils/voxel_io.cpp.i
.PHONY : src/utils/voxel_io.i

# target to preprocess a source file
src/utils/voxel_io.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_io.dir/build.make CMakeFiles/projectV-voxel_io.dir/src/utils/voxel_io.cpp.i
.PHONY : src/utils/voxel_io.cpp.i

src/utils/voxel_io.s: src/utils/voxel_io.cpp.s
.PHONY : src/utils/voxel_io.s

# target to generate assembly for a file
src/utils/voxel_io.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_io.dir/build.make CMakeFiles/projectV-voxel_io.dir/src/utils/voxel_io.cpp.s
.PHONY : src/utils/voxel_io.cpp.s

src/utils/voxel_management.o: src/utils/voxel_management.cpp.o
.PHONY : src/utils/voxel_management.o

# target to build an object file
src/utils/voxel_management.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_management.dir/build.make CMakeFiles/projectV-voxel_management.dir/src/utils/voxel_management.cpp.o
.PHONY : src/utils/voxel_management.cpp.o

src/utils/voxel_management.i: src/utils/voxel_management.cpp.i
.PHONY : src/utils/voxel_management.i

# target to preprocess a source file
src/utils/voxel_management.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_management.dir/build.make CMakeFiles/projectV-voxel_management.dir/src/utils/voxel_management.cpp.i
.PHONY : src/utils/voxel_management.cpp.i

src/utils/voxel_management.s: src/utils/voxel_management.cpp.s
.PHONY : src/utils/voxel_management.s

# target to generate assembly for a file
src/utils/voxel_management.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_management.dir/build.make CMakeFiles/projectV-voxel_management.dir/src/utils/voxel_management.cpp.s
.PHONY : src/utils/voxel_management.cpp.s

src/utils/voxel_math.o: src/utils/voxel_math.cpp.o
.PHONY : src/utils/voxel_math.o

# target to build an object file
src/utils/voxel_math.cpp.o:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_math.dir/build.make CMakeFiles/projectV-voxel_math.dir/src/utils/voxel_math.cpp.o
.PHONY : src/utils/voxel_math.cpp.o

src/utils/voxel_math.i: src/utils/voxel_math.cpp.i
.PHONY : src/utils/voxel_math.i

# target to preprocess a source file
src/utils/voxel_math.cpp.i:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_math.dir/build.make CMakeFiles/projectV-voxel_math.dir/src/utils/voxel_math.cpp.i
.PHONY : src/utils/voxel_math.cpp.i

src/utils/voxel_math.s: src/utils/voxel_math.cpp.s
.PHONY : src/utils/voxel_math.s

# target to generate assembly for a file
src/utils/voxel_math.cpp.s:
	$(MAKE) $(MAKESILENT) -f CMakeFiles/projectV-voxel_math.dir/build.make CMakeFiles/projectV-voxel_math.dir/src/utils/voxel_math.cpp.s
.PHONY : src/utils/voxel_math.cpp.s

# Help Target
help:
	@echo "The following are some of the valid targets for this Makefile:"
	@echo "... all (the default if no target is provided)"
	@echo "... clean"
	@echo "... depend"
	@echo "... edit_cache"
	@echo "... rebuild_cache"
	@echo "... projectV-ecs"
	@echo "... projectV-fbo"
	@echo "... projectV-lod"
	@echo "... projectV-render"
	@echo "... projectV-scene"
	@echo "... projectV-shader"
	@echo "... projectV-uniforms"
	@echo "... projectV-user_input"
	@echo "... projectV-voxel_io"
	@echo "... projectV-voxel_management"
	@echo "... projectV-voxel_math"
	@echo "... projectV-window"
	@echo "... src/core/ecs.o"
	@echo "... src/core/ecs.i"
	@echo "... src/core/ecs.s"
	@echo "... src/graphics/fbo.o"
	@echo "... src/graphics/fbo.i"
	@echo "... src/graphics/fbo.s"
	@echo "... src/graphics/render.o"
	@echo "... src/graphics/render.i"
	@echo "... src/graphics/render.s"
	@echo "... src/graphics/scene.o"
	@echo "... src/graphics/scene.i"
	@echo "... src/graphics/scene.s"
	@echo "... src/graphics/shader.o"
	@echo "... src/graphics/shader.i"
	@echo "... src/graphics/shader.s"
	@echo "... src/graphics/uniforms.o"
	@echo "... src/graphics/uniforms.i"
	@echo "... src/graphics/uniforms.s"
	@echo "... src/graphics/user_input.o"
	@echo "... src/graphics/user_input.i"
	@echo "... src/graphics/user_input.s"
	@echo "... src/graphics/window.o"
	@echo "... src/graphics/window.i"
	@echo "... src/graphics/window.s"
	@echo "... src/utils/lod.o"
	@echo "... src/utils/lod.i"
	@echo "... src/utils/lod.s"
	@echo "... src/utils/voxel_io.o"
	@echo "... src/utils/voxel_io.i"
	@echo "... src/utils/voxel_io.s"
	@echo "... src/utils/voxel_management.o"
	@echo "... src/utils/voxel_management.i"
	@echo "... src/utils/voxel_management.s"
	@echo "... src/utils/voxel_math.o"
	@echo "... src/utils/voxel_math.i"
	@echo "... src/utils/voxel_math.s"
.PHONY : help



#=============================================================================
# Special targets to cleanup operation of make.

# Special rule to run CMake to check the build system integrity.
# No rule that depends on this can have commands that come from listfiles
# because they might be regenerated.
cmake_check_build_system:
	$(CMAKE_COMMAND) -S$(CMAKE_SOURCE_DIR) -B$(CMAKE_BINARY_DIR) --check-build-system CMakeFiles/Makefile.cmake 0
.PHONY : cmake_check_build_system

