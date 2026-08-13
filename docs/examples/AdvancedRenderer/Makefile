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
    -lprojectV-scene_query \
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
# scene report (bounds, chunk counts, load failures) -- which is how a stale scene tells you it is
# stale instead of rendering an empty sky.
#
# EXTRA_CXXFLAGS is for a one-off build without editing this file. The frame timer in update():
#   make clean && make EXTRA_CXXFLAGS=-DPROJV_ENABLE_PERF
CXXFLAGS := -O3 --std=c++17 -Wall -Wextra \
    -DPROJV_ENABLE_INFO -DPROJV_ENABLE_WARN -DPROJV_ENABLE_ERROR \
    $(INCLUDE_DIRS) $(EXTRA_CXXFLAGS)
LDFLAGS := $(LIB_DIR) $(USED_LIBRARIES) $(BGFX_LIBRARIES)

# ========================
# Sources
# ========================
TARGET := advanced_renderer
OBJECT_DIR := obj
SOURCES := main.cpp
OBJECTS := $(addprefix $(OBJECT_DIR)/,$(SOURCES:.cpp=.o))
# -MMD -MP writes a .d beside each .o listing every header it included, engine headers included, and
# -include pulls those back in. Without it `make` reports the target up to date after an engine
# header changes -- the example then links against a stale object whose idea of a struct's layout no
# longer matches the library's, which fails at run time rather than at build time.
DEPENDENCIES := $(OBJECTS:.o=.d)

# ========================
# Targets
# ========================
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(OBJECT_DIR)/%.o: %.cpp | $(OBJECT_DIR)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJECT_DIR):
	mkdir -p $(OBJECT_DIR)

-include $(DEPENDENCIES)

# ========================
# Clean
# ========================
.PHONY: all clean

clean:
	rm -rf $(TARGET) $(OBJECT_DIR)
