# If RACK_DIR is not defined when calling the Makefile, default to the sibling
# Rack SDK layout used by a standard local Rack checkout.
RACK_DIR ?= ../Rack-SDK
PYTHON ?= python3

# sfizioso and the SFZ module require C++17. EXTRA_CXXFLAGS is appended after
# Rack's default language-standard flag, so it wins on every Rack toolchain.
EXTRA_CXXFLAGS += -std=c++17
CXXFLAGS += -Ideps/sfizioso/src

SOURCES += src/plugin.cpp
SOURCES += src/SFZ.cpp
SOURCES += $(wildcard src/sfz/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

# License and notice files for sfizioso and every bundled component compiled
# into the static library.
SFIZZ_LICENSE_FILES := \
	deps/sfizioso/LICENSE \
	deps/sfizioso/NOTICE \
	deps/sfizioso/AUTHORS.md \
	deps/sfizioso/external/abseil-cpp/LICENSE \
	deps/sfizioso/external/atomic_queue/LICENSE \
	deps/sfizioso/external/cephes/LICENSE.txt \
	deps/sfizioso/external/filesystem/LICENSE \
	deps/sfizioso/external/invoke.hpp/LICENSE.md \
	deps/sfizioso/external/jsl/LICENSE.md \
	deps/sfizioso/external/simde/COPYING \
	deps/sfizioso/external/st_audiofile/LICENSE.md \
	deps/sfizioso/external/st_audiofile/thirdparty/dr_libs/LICENSE \
	deps/sfizioso/external/st_audiofile/thirdparty/libaiff/LICENSE \
	deps/sfizioso/external/st_audiofile/thirdparty/stb_vorbis/LICENSE \
	deps/sfizioso/external/st_audiofile/thirdparty/wavpack/COPYING \
	deps/sfizioso/external/st_audiofile/thirdparty/wavpack/license.txt \
	deps/sfizioso/external/threadpool/ThreadPool.h \
	deps/sfizioso/src/external/cpuid/LICENSE.rst \
	deps/sfizioso/src/external/cpuid/platform/LICENSE.rst \
	deps/sfizioso/src/external/hiir/license.txt \
	deps/sfizioso/src/external/kiss_fft/COPYING \
	deps/sfizioso/src/external/pugixml/LICENSE.md \
	deps/sfizioso/src/external/spline/LICENSE \
	deps/sfizioso/src/external/tunings/LICENSE.md
DISTRIBUTABLES += $(SFIZZ_LICENSE_FILES)

include $(RACK_DIR)/plugin.mk

# Pinned, headless Cella sfizioso fork. Run `make sfz-bootstrap` after a
# recursive checkout to verify the dependency before the first build.
SFIZZ_RACK_PLUGIN_DIR := .
SFIZZ_USE_SNDFILE := 0
SFIZZ_USE_OPENMP := 0
SFIZZ_DISABLE_TIMING := 1
include deps/sfizioso/rack.mk
LDFLAGS += $(SFIZZ_LINK_FLAGS)
$(TARGET): $(SFIZZ_TARGET)

SFZ_CORE_TEST := build/sfz_core_test
SFZ_MODULE_TEST := build/sfz_module_test

ifdef ARCH_MAC
# Newer macOS versions can reject the SDK's ad-hoc libRack signature in a test
# executable. Prefer the matching installed Rack library when available.
RACK_TEST_LIB_DIR ?= $(shell \
	if [ -f '/Applications/VCV Rack 2 Free.app/Contents/Resources/libRack.dylib' ]; then \
		printf '%s' '/Applications/VCV Rack 2 Free.app/Contents/Resources'; \
	elif [ -f '/Applications/VCV Rack 2 Pro.app/Contents/Resources/libRack.dylib' ]; then \
		printf '%s' '/Applications/VCV Rack 2 Pro.app/Contents/Resources'; \
	else \
		printf '%s' '$(RACK_DIR)'; \
	fi)
endif

$(SFZ_CORE_TEST): tests/sfz_core_test.cpp $(wildcard src/sfz/*.cpp) $(wildcard src/sfz/*.hpp) $(SFIZZ_TARGET)
	@mkdir -p $(@D)
	$(CXX) -std=c++17 -O2 -g -Isrc -Ideps/sfizioso/src $(SFIZZ_CXX_FLAGS) \
		-o $@ tests/sfz_core_test.cpp $(wildcard src/sfz/*.cpp) \
		$(SFIZZ_TARGET) $(SFIZZ_LINK_FLAGS)

$(SFZ_MODULE_TEST): tests/sfz_module_test.cpp src/SFZ.cpp $(wildcard src/sfz/*.cpp) $(wildcard src/sfz/*.hpp) $(SFIZZ_TARGET)
	@mkdir -p $(@D)
	$(CXX) -std=c++17 -O2 -g $(FLAGS) -Isrc -Ideps/sfizioso/src $(SFIZZ_CXX_FLAGS) \
		-o $@ tests/sfz_module_test.cpp $(wildcard src/sfz/*.cpp) \
		$(SFIZZ_TARGET) $(SFIZZ_LINK_FLAGS) -L$(RACK_DIR) -lRack

.PHONY: sfz-bootstrap sfz-panel sfz-panel-check sfz-core-test sfz-module-test sfz-test
sfz-bootstrap:
	tools/sfz/bootstrap.sh

sfz-panel:
	$(PYTHON) tools/generate_sfz_panels.py

sfz-panel-check:
	$(PYTHON) tools/generate_sfz_panels.py --check

sfz-core-test: $(SFZ_CORE_TEST)
	$(SFZ_CORE_TEST)

sfz-module-test: $(SFZ_MODULE_TEST)
ifdef ARCH_MAC
	DYLD_LIBRARY_PATH="$(RACK_TEST_LIB_DIR)" $(SFZ_MODULE_TEST)
else ifdef ARCH_LIN
	LD_LIBRARY_PATH=$(RACK_DIR) $(SFZ_MODULE_TEST)
else
	$(SFZ_MODULE_TEST)
endif

sfz-test: sfz-panel-check sfz-core-test sfz-module-test
