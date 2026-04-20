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
USED_LIBRARIES := \
    -lprojectV-ecs \
    -lprojectV-lod \
    -lprojectV-voxel_io \
    -lprojectV-voxel_management \
    -lprojectV-voxel_math \
	-lprojectV-math \
	-lprojectV-disk_io \
	-lprojectV-gpu_interface \
	-lprojectV-manage_resources \
	-lprojectV-perform_renderer \
	-lprojectV-type_mapping \
	-lprojectV-render_instance

# ========================
# Compiler and Flags
# ========================
CXX ?= g++
CXXFLAGS := -O3 --std=c++17 $(INCLUDE_DIRS)
LDFLAGS := $(LIB_DIR) $(USED_LIBRARIES)
# ========================
# Targets
# ========================
TARGET := main.o
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
