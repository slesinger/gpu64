# gpu64 rastercheck -- builds the firmware's class 2 raster core with a native
# g++ so tools/rastercheck/check.py can diff it against the reference model.
#
#   make            build ./rastercheck
#   make run        build and run the differential suite
#
# Nothing here is part of the firmware build; tools/build.sh ignores it.

FW      := ../../Source/Firmware
CXX     ?= g++
CXXFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter \
            -DGPU64_HOSTSIM -I ../hostsim/stub -I $(FW)

SRCS := rastercheck.cpp $(FW)/gpu64_raster_core.cpp

rastercheck: $(SRCS) $(FW)/gpu64_raster_core.h
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

run: rastercheck
	python3 check.py

clean:
	rm -f rastercheck
	rm -rf out

.PHONY: run clean
