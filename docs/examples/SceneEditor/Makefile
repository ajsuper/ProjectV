# ========================
# Project Paths
# ========================
PROJECTV_DIR := ../../../

BGFX_DIR := $(PROJECTV_DIR)/external/bgfx/
BX_DIR := $(PROJECTV_DIR)/external/bx/
BIMG_DIR := $(PROJECTV_DIR)/external/bimg/
IMGUI_DIR := $(PROJECTV_DIR)/external/imgui/

INCLUDE_DIRS := \
    -I$(PROJECTV_DIR)/include \
    -I$(PROJECTV_DIR)/external/spdlog/include \
    -I$(PROJECTV_DIR)/external/json/include \
    -I$(PROJECTV_DIR)/external/glm \
    -I$(IMGUI_DIR) \
    -I$(IMGUI_DIR)backends \
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
	-lprojectV-picking \
	-lprojectV-editing \
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
# editor's scene report (bounds, chunk counts, load failures).
CXXFLAGS := -O2 --std=c++17 -Wall -Wextra \
	-DPROJV_ENABLE_INFO -DPROJV_ENABLE_WARN -DPROJV_ENABLE_ERROR \
	$(INCLUDE_DIRS)
LDFLAGS := $(LIB_DIR) $(USED_LIBRARIES) $(BGFX_LIBRARIES)

# ========================
# Sources
# ========================
TARGET := scene_editor
OBJECT_DIR := obj

# Dear ImGui is compiled straight into the example. It lives in the engine's external/ (it is meant
# to become part of the engine proper later), but no engine library links against it yet, so nothing
# is gained by building it separately.
IMGUI_SOURCES := \
	$(IMGUI_DIR)imgui.cpp \
	$(IMGUI_DIR)imgui_draw.cpp \
	$(IMGUI_DIR)imgui_tables.cpp \
	$(IMGUI_DIR)imgui_widgets.cpp \
	$(IMGUI_DIR)backends/imgui_impl_glfw.cpp

EDITOR_SOURCES := main.cpp imgui_impl_bgfx.cpp edit_history.cpp

# ImGui's objects are named after their basename only; the two source trees have no name collisions.
OBJECTS := $(addprefix $(OBJECT_DIR)/,$(notdir $(IMGUI_SOURCES:.cpp=.o))) \
           $(addprefix $(OBJECT_DIR)/,$(EDITOR_SOURCES:.cpp=.o))

# ========================
# Targets
# ========================
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

$(OBJECT_DIR)/%.o: %.cpp | $(OBJECT_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJECT_DIR)/%.o: $(IMGUI_DIR)%.cpp | $(OBJECT_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJECT_DIR)/%.o: $(IMGUI_DIR)backends/%.cpp | $(OBJECT_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJECT_DIR):
	mkdir -p $(OBJECT_DIR)

# ========================
# Clean
# ========================
.PHONY: all clean

clean:
	rm -rf $(TARGET) $(OBJECT_DIR)
