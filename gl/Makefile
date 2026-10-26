CXX ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -fPIC -Iinclude -Isrc
LDFLAGS ?= -ldl

BUILD := build
LIB := $(BUILD)/libpyrowave_gl.so
TEST := $(BUILD)/pwgl_test

.PHONY: all clean test

all: $(LIB) $(TEST)

$(BUILD):
	mkdir -p $(BUILD)

$(LIB): src/gl_api.cpp src/encoder.cpp src/gl_api.hpp src/dwt.glsl.inl src/quant.glsl.inl src/pack.glsl.inl include/pyrowave_gl.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -shared -DPWGL_BUILD_DLL src/gl_api.cpp src/encoder.cpp -o $@ $(LDFLAGS)

$(TEST): src/test_encode.cpp $(LIB) | $(BUILD)
	$(CXX) $(CXXFLAGS) src/test_encode.cpp -o $@ -L$(BUILD) -lpyrowave_gl $(LDFLAGS) -Wl,-rpath,'$$ORIGIN'

test: $(TEST)
	$(TEST)

clean:
	rm -rf $(BUILD)
