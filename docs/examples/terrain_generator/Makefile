# ========================
# Project Paths
# ========================
PROJECTV_DIR := ../../..
BGFX_DIR := $(PROJECTV_DIR)/external/bgfx/
BX_DIR := $(PROJECTV_DIR)/external/bx/
BIMG_DIR := $(PROJECTV_DIR)/external/bimg/

INCLUDE_DIRS := \
    -I./include \
    -I$(PROJECTV_DIR)/include \
    -I$(PROJECTV_DIR)/external/spdlog/include \
    -I$(PROJECTV_DIR)/external/json/include \
    -I$(PROJECTV_DIR)/external/glm \
    -I$(BX_DIR)include \
    -I$(BGFX_DIR)include \
    -I$(BIMG_DIR)include \

LIB_DIR := -L$(PROJECTV_DIR)/lib \
           -L$(BGFX_DIR).build/linux64_gcc/bin/

# ========================
# Libraries
# ========================
USED_LIBRARIES := \
    -lprojectV-editing \
    -lprojectV-ecs \
    -lprojectV-compose_io \
    -lprojectV-scene_query \
    -lprojectV-voxel_management \
    -lprojectV-material \
    -lprojectV-voxel_math \
    -lprojectV-math \
    -lprojectV-disk_io \
    -lprojectV-gpu_interface \
    -lprojectV-manage_resources \
    -lprojectV-perform_renderer \
    -lprojectV-type_mapping \
    -lprojectV-render_instance

BGFX_LIBRARIES := \
    -lbgfxDebug \
    -lbimgDebug \
    -lbxDebug \
    -lglfw \
    -ldl

# ========================
# Compiler and Flags
# ========================
CXX ?= g++
CXXFLAGS := -O3 --std=c++17 -march=native -DPROJV_ENABLE_INFO -DPROJV_ENABLE_PERF $(INCLUDE_DIRS)
LDFLAGS := $(LIB_DIR) $(USED_LIBRARIES) $(BGFX_LIBRARIES)

# ========================
# Targets
# ========================
TARGET := terrain_generator
SRC := main.cpp
# The generator's own modules (terrain_noise, tree_placement) are header-only, so they must be
# prerequisites too: with only main.cpp listed, editing a header leaves `make` believing the target
# is up to date and silently keeps running the previous binary.
HEADERS := $(wildcard include/*.hpp) $(wildcard include/*.h)

all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

.PHONY: all clean

clean:
	rm -f $(TARGET)
