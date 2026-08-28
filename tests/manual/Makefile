PROJECTV_DIR := ../../

INCLUDE_DIRS := \
    -I$(PROJECTV_DIR)/include \
    -I$(PROJECTV_DIR)/external/spdlog/include \
    -I$(PROJECTV_DIR)/external/json/include \
    -I$(PROJECTV_DIR)/external/glm \
    -I$(PROJECTV_DIR)/external/bx/include \
    -I$(PROJECTV_DIR)/external/bgfx/include \
    -I$(PROJECTV_DIR)/external/bimg/include

LIB_DIR := -L$(PROJECTV_DIR)/build/lib

USED_LIBRARIES := -lprojectV

BGFX_LIBRARIES := -lbgfx -lbimg -lbx -lglfw

CXX ?= g++
CXXFLAGS := -O2 --std=c++17 -Wall -Wextra \
	-DPROJV_ENABLE_INFO -DPROJV_ENABLE_WARN -DPROJV_ENABLE_ERROR $(INCLUDE_DIRS)
LDFLAGS := $(LIB_DIR) $(USED_LIBRARIES) $(BGFX_LIBRARIES)

all: exit_path paths_probe

# Depend on the engine archives, not just the source. Without this, make reports a test as
# up to date after only the engine changed -- which silently turns a regression run into a
# false PASS, as happened while these tests were being written.
ENGINE_LIBS := $(wildcard $(PROJECTV_DIR)/build/lib/libprojectV.a)

exit_path: exit_path.cpp $(ENGINE_LIBS)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

paths_probe: paths_probe.cpp $(ENGINE_LIBS)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

.PHONY: all clean
clean:
	rm -f exit_path paths_probe
