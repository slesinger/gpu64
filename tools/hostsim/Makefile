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
        $(FW)/gpu64_3d_render.cpp

hostsim: $(SRCS) $(FW)/gpu64_3d_render.h $(FW)/gpu64_3d_math.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

run: hostsim
	mkdir -p out
	./hostsim out

clean:
	rm -f hostsim
	rm -rf out

.PHONY: run clean
