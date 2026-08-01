# ========================
# Project Paths
# ========================
PROJECTV_DIR := ../../../

BGFX_DIR := $(PROJECTV_DIR)/external/bgfx/
BX_DIR := $(PROJECTV_DIR)/external/bx/
BIMG_DIR := $(PROJECTV_DIR)/external/bimg/

INCLUDE_DIRS := \
    -I$(PROJECTV_DIR)/include \
    -I$(PROJECTV_DIR)/external/spdlog/include \
    -I$(PROJECTV_DIR)/external/json/include \
    -I$(PROJECTV_DIR)/external/glm \
	-I$(BX_DIR)include \
	-I$(BGFX_DIR)include \
	-I$(BIMG_DIR)include \

LIB_DIR := -L$(PROJECTV_DIR)/lib \
					 -L$(PROJECTV_DIR)external/bgfx/.build/linux64_gcc/bin/ \

# ========================
# Libraries
# ========================
# Wrapped in a link group: compose_io/voxel_management and material reference each other
# (brickMapFromVoxelTypeData -> internMaterial, and back), which a single pass over static
# archives cannot resolve regardless of ordering.
USED_LIBRARIES := \
	-Wl,--start-group \
    -lprojectV-ecs \
    -lprojectV-compose_io \
    -lprojectV-voxel_management \
    -lprojectV-material \
    -lprojectV-voxel_math \
	-lprojectV-math \
	-lprojectV-disk_io \
	-lprojectV-gpu_interface \
	-lprojectV-manage_resources \
	-lprojectV-perform_renderer \
	-lprojectV-type_mapping \
	-lprojectV-render_instance \
	-Wl,--end-group

BGFX_LIBRARIES := \
		 -lbgfxRelease \
		 -lbimgRelease \
		 -lbxRelease \
		 -lglfw \

# ========================
# Compiler and Flags
# ========================
CXX ?= g++
# The log wrapper compiles each level out unless its macro is defined. INFO/WARN/ERROR carry the
# previewer's scene report (bounds, chunk counts, framing); PERF enables the frame-time profiler.
CXXFLAGS := -O3 --std=c++17 -Wall -Wextra \
	-DPROJV_ENABLE_INFO -DPROJV_ENABLE_WARN -DPROJV_ENABLE_ERROR -DPROJV_ENABLE_PERF \
	$(INCLUDE_DIRS)
LDFLAGS := $(LIB_DIR) $(USED_LIBRARIES) $(BGFX_LIBRARIES)

# ========================
# Targets
# ========================
TARGET := scene_previewer
SRC := main.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# ========================
# Clean
# ========================
.PHONY: all clean

clean:
	rm -f $(TARGET)
