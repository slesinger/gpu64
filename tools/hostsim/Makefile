# gpu64 host sim -- builds the portable half of the milestone 6 renderer with
# a native g++ so the pipeline can be debugged on a PC.
#
#   make            build ./hostsim
#   make run        build, render, and drop PPMs in out/
#
# Nothing here is part of the firmware build; tools/build.sh does not look at
# this directory.

FW      := ../../Source/Firmware
CXX     ?= g++
CXXFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter \
            -DGPU64_HOSTSIM -I stub -I $(FW)

SRCS := hostsim.cpp \
        $(FW)/gpu64_3d_math.cpp \
        $(FW)/gpu64_3d_raster.cpp \
        $(FW)/gpu64_3d_colormap.cpp \
        $(FW)/gpu64_3d_render.cpp \
        $(FW)/gpu64_3d_scene.cpp

SCENESRCS := scenesim.cpp \
        $(FW)/gpu64_3d_math.cpp \
        $(FW)/gpu64_3d_raster.cpp \
        $(FW)/gpu64_3d_colormap.cpp \
        $(FW)/gpu64_3d_render.cpp \
        $(FW)/gpu64_3d_scene.cpp

HDRS := $(FW)/gpu64_3d_render.h $(FW)/gpu64_3d_math.h $(FW)/gpu64_3d_scene.h

all: hostsim scenesim

hostsim: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

# The class-1 half: renders the frame stream tools/prgsim emits, so a demo
# built on the scene graph gets a picture on a PC the way an immediate-mode
# one gets it from hostsim.
scenesim: $(SCENESRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(SCENESRCS)

run: hostsim
	mkdir -p out
	./hostsim out

clean:
	rm -f hostsim scenesim
	rm -rf out

.PHONY: all run clean
