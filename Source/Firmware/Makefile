#
# Makefile
#

#EXTRACLEAN =
CIRCLEHOME = ../..

OBJS = rad_main.o dirscan.o config.o rad_reu.o rad_hijack.o lowlevel_arm64.o gpio_defs.o helpers.o lowlevel_dma.o rad_iecdevice.o gpu64_multicore.o gpu64_fb.o gpu64_api.o gpu64_vsync.o gpu64_flip.o gpu64_ladder.o gpu64_3d_core1.o gpu64_3d_class1.o gpu64_3d_math.o gpu64_3d_raster.o gpu64_3d_colormap.o gpu64_3d_render.o
LIBS =  $(CIRCLEHOME)/addon/linux/liblinuxemu.a

CFLAGS += -fno-threadsafe-statics 
CFLAGS += -Wno-comment -Ofast -g0 

LIBS += $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	    $(CIRCLEHOME)/lib/fs/libfs.a \
		$(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	    $(CIRCLEHOME)/lib/sched/libsched.a \
		$(CIRCLEHOME)/lib/usb/libusb.a \
		$(CIRCLEHOME)/lib/input/libinput.a \
        $(CIRCLEHOME)/lib/libcircle.a

include ../Rules.mk

-include $(DEPS)