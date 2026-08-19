# sim/verilator/Makefile ------------------------------------------------------
MODULE ?= regfile

REPO   := $(abspath ../..)
RTL    := $(REPO)/rtl
TB_DIR := $(REPO)/sim/verilator
WAVE_D := $(REPO)/sim/waves
OBJ    := $(TB_DIR)/obj_dir/$(MODULE)

# Per-module source lists. Default (below) is rtl/core/<MODULE>.v; add an entry
# here only when a module needs extra files (e.g. a package or a submodule).
RTL_selftest := $(TB_DIR)/selftest/selftest.v
RTL_core := $(RTL)/core/core.v $(RTL)/core/regfile.v $(RTL)/core/alu.v \
            $(RTL)/core/immgen.v $(RTL)/core/decoder.v $(RTL)/core/imem.v \
            $(RTL)/core/dmem.v $(RTL)/core/branch_cmp.v $(RTL)/core/csr.v

RTL_SRCS := $(if $(RTL_$(MODULE)),$(RTL_$(MODULE)),$(RTL)/core/$(MODULE).v)
TB_SRC   := $(TB_DIR)/tb_$(MODULE).cpp
EXE      := $(OBJ)/V$(MODULE)

MODULES  := regfile alu immgen decoder imem dmem

VFLAGS := --cc --exe --build -j 0 \
          --top-module $(MODULE) \
          -Mdir $(OBJ) \
          -I$(RTL)/include \
          --timescale 1ns/1ps \
          --trace-fst --trace-structs \
          -Wall -Wno-DECLFILENAME \
          -CFLAGS "-std=c++17 -O2 -g -I$(TB_DIR)" \
          -o V$(MODULE)

.PHONY: all run waves lint clean test-all help

all: run

$(EXE): $(RTL_SRCS) $(TB_SRC) $(TB_DIR)/tb_common.h
	@mkdir -p $(OBJ) $(WAVE_D)
	verilator $(VFLAGS) $(RTL_SRCS) $(TB_SRC)

run: $(EXE)
	@cd $(WAVE_D) && $(EXE) $(TBARGS)

waves: run
	gtkwave $(WAVE_D)/$(MODULE).fst $(wildcard $(WAVE_D)/$(MODULE).gtkw) &

lint:
	verilator --lint-only -Wall -I$(RTL)/include \
	          --top-module $(MODULE) $(RTL_SRCS)

test-all:
	@for m in $(MODULES); do \
	   echo "=== $$m ==="; $(MAKE) --no-print-directory MODULE=$$m run || exit 1; \
	 done
	@echo "ALL MODULES PASS"

clean:
	rm -rf $(TB_DIR)/obj_dir $(WAVE_D)/*.fst $(WAVE_D)/*.vcd

help:
	@echo "make [run|waves|lint|clean] MODULE=<name>   TBARGS='+seed=1234'"
	@echo "make test-all"
