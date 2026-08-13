# ========================
# Project Paths
# ========================
PROJECTV_DIR := ../../../

BGFX_DIR := $(PROJECTV_DIR)/external/bgfx/
BX_DIR := $(PROJECTV_DIR)/external/bx/
BIMG_DIR := $(PROJECTV_DIR)/external/bimg/
IMGUI_DIR := $(PROJECTV_DIR)/external/imgui/
# Lua lives in *this example's* external/, not the engine's: nothing in the engine embeds a
# scripting language, and the brush system is the editor's feature rather than the renderer's.
LUA_DIR := external/lua/

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
	-I$(LUA_DIR) \

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
CC ?= gcc
# The log wrapper compiles each level out unless its macro is defined. INFO/WARN/ERROR carry the
# editor's scene report (bounds, chunk counts, load failures).
CXXFLAGS := -O2 --std=c++17 -Wall -Wextra \
	-DPROJV_ENABLE_INFO -DPROJV_ENABLE_WARN -DPROJV_ENABLE_ERROR \
	$(INCLUDE_DIRS)
# Lua is C, and it is compiled with LUA_USE_POSIX rather than LUA_USE_LINUX on purpose: the Linux
# preset adds LUA_USE_DLOPEN, which gives a script `package.loadlib` and with it the ability to load
# arbitrary native code. A brush is a file in a folder the user may have copied from anywhere, so the
# one capability the sandbox cannot take back is the one not compiled in. POSIX still gives Lua the
# clock and locale it wants. -w because Lua's own warnings are not this project's to fix.
LUA_CFLAGS := -O2 -DLUA_USE_POSIX -w -I$(LUA_DIR)
# -lm is Lua's, and explicit: it uses pow/fmod/floor from the C library and is the only part of this
# binary that would go looking for them if bgfx happened to stop pulling libm in.
LDFLAGS := $(LIB_DIR) $(USED_LIBRARIES) $(BGFX_LIBRARIES) -lm

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

EDITOR_SOURCES := main.cpp imgui_impl_bgfx.cpp edit_history.cpp brush.cpp

# Lua, compiled straight into the editor like ImGui. Every .c in the tree except the three that are
# not part of the library: lua.c is the standalone interpreter's main(), onelua.c is the
# everything-in-one-translation-unit build, and ltests.c is the internal test harness (it needs
# LUA_DEBUG and defines its own allocator). Wildcarded rather than listed so a Lua point release
# that adds a file does not need this edited.
LUA_SOURCES := $(filter-out $(LUA_DIR)lua.c $(LUA_DIR)onelua.c $(LUA_DIR)ltests.c, \
                            $(wildcard $(LUA_DIR)*.c))

# ImGui's objects are named after their basename only; the two source trees have no name collisions.
# Lua's go in a subdirectory of their own, because they do collide -- Lua has an lstate.c and bgfx
# has nothing to do with it, but ImGui's flattening convention leaves no room to be sure.
OBJECTS := $(addprefix $(OBJECT_DIR)/,$(notdir $(IMGUI_SOURCES:.cpp=.o))) \
           $(addprefix $(OBJECT_DIR)/,$(EDITOR_SOURCES:.cpp=.o)) \
           $(patsubst $(LUA_DIR)%.c,$(OBJECT_DIR)/lua/%.o,$(LUA_SOURCES))

# ========================
# Targets
# ========================
all: $(TARGET)

# The submodule is a clone the user has to have asked for, and a missing one otherwise shows up as
# forty "lua.h: No such file" errors from a header nobody in this repo wrote.
$(LUA_DIR)lua.h:
	@echo "external/lua is empty -- run: git submodule update --init docs/examples/SceneEditor/external/lua" >&2
	@false

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS)

# -MMD -MP writes a .d beside each object listing the headers it actually included, and the -include
# at the bottom feeds them back as rules. Without it this Makefile knows only that main.cpp makes
# main.o, so an edit to an engine header -- or to brush.h -- left `make` reporting nothing to do and
# the binary silently built against the old declarations.
DEPFLAGS := -MMD -MP

$(OBJECT_DIR)/%.o: %.cpp $(LUA_DIR)lua.h | $(OBJECT_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJECT_DIR)/%.o: $(IMGUI_DIR)%.cpp | $(OBJECT_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJECT_DIR)/%.o: $(IMGUI_DIR)backends/%.cpp | $(OBJECT_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJECT_DIR)/lua/%.o: $(LUA_DIR)%.c | $(OBJECT_DIR)/lua
	$(CC) $(LUA_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(OBJECT_DIR):
	mkdir -p $(OBJECT_DIR)

$(OBJECT_DIR)/lua:
	mkdir -p $(OBJECT_DIR)/lua

# ========================
# Clean
# ========================
.PHONY: all clean

clean:
	rm -rf $(TARGET) $(OBJECT_DIR)

# Last, and with a leading dash: on the first build there are no .d files yet, and make must not
# treat that as an error.
-include $(OBJECTS:.o=.d)
