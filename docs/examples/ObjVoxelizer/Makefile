# ========================
# Project Paths
# ========================
PROJECTV_DIR := ../../../

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
					 -L$(PROJECTV_DIR)external/bgfx/.build/linux64_gcc/bin/ \

# ========================
# Libraries
# ========================
USED_LIBRARIES := \
    -lprojectV-compose_io \
    -lprojectV-voxel_management \
    -lprojectV-material \
    -lprojectV-voxel_math \
    -lprojectV-math

# ========================
# Compiler and Flags
# ========================
CXX ?= g++
# The log wrapper compiles each level out unless its macro is defined; this tool's entire
# user-facing output goes through info/warn/error, so all three have to be on.
CXXFLAGS := -O3 --std=c++17 -DPROJV_ENABLE_INFO -DPROJV_ENABLE_WARN -DPROJV_ENABLE_ERROR $(INCLUDE_DIRS)
LDFLAGS := $(LIB_DIR) $(USED_LIBRARIES)
# ========================
# Targets
# ========================
TARGET := obj_voxelizer
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
