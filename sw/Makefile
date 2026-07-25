CROSS_COMPILE ?= riscv-none-elf-
CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump

ARCH ?= rv32i_zicsr
ABI  ?= ilp32

CFLAGS  := -march=$(ARCH) -mabi=$(ABI) -O2 -g -Wall \
           -ffreestanding -fno-builtin -nostdlib
LDFLAGS := -static -nostartfiles -T common/link.ld \
           -Wl,--no-warn-rwx-segments
LIBS    := -lgcc

PROGS := $(basename $(notdir $(wildcard examples/*.c)))

all: $(addprefix build/,$(addsuffix .hex,$(PROGS)))

build:
	mkdir -p build

build/%.elf: examples/%.c common/crt0.S common/link.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) common/crt0.S $< $(LIBS) -o $@
	$(OBJDUMP) -d $@ > build/$*.dis

build/%.bin: build/%.elf
	$(OBJCOPY) -O binary $< $@

build/%.hex: build/%.bin
	python3 common/makehex.py $< > $@

clean:
	rm -rf build

.PRECIOUS: build/%.elf build/%.bin