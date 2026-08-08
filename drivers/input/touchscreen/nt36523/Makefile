#
# Makefile for the Novatek NT36523 touchscreen driver.
# NT36675 (Xiaomi Apollo) MP self-test support added from vendor driver.
#

# Each configuration option enables a list of files.
obj-$(CONFIG_TOUCHSCREEN_NT36523_SPI) += nt36523_ts.o
nt36523_ts-y := nt36xxx.o \
	nt36xxx_fw_update.o

# MP (multipoint) self-test firmware support (vendor NT36675 port).
# Enabled when NVT_TOUCH_MP=1 in nt36xxx.h (default: on).
nt36523_ts-y += nt36xxx_mp_ctrlram.o \
	nt36xxx_mp_ctrlram_data.o
