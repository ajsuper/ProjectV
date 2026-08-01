# The voxelizer is built by CMake — see CMakeLists.txt for why (Assimp is a CMake project, and
# building it as a subproject is the only mesh-import story identical on Linux, macOS and Windows).
# This Makefile is a convenience wrapper so `make` still does the obvious thing.

BUILD_DIR := build
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
ASSIMP_DIR := external/assimp

.PHONY: all submodule clean distclean

all: submodule
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) -j$(JOBS)

# Assimp is a submodule of this example; a fresh checkout has the directory but not the contents.
submodule:
	@if [ ! -f $(ASSIMP_DIR)/CMakeLists.txt ]; then \
		echo "Fetching the Assimp submodule..."; \
		git -C ../../.. submodule update --init --recursive docs/examples/MeshVoxelizer/$(ASSIMP_DIR); \
	fi

# Drops the tool but keeps the (slow to rebuild) Assimp objects.
clean:
	rm -f mesh_voxelizer
	@if [ -d $(BUILD_DIR) ]; then cmake --build $(BUILD_DIR) --target clean; fi

distclean:
	rm -rf $(BUILD_DIR) mesh_voxelizer
