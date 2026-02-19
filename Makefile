SHELL := bash
.ONESHELL:
.SECONDEXPANSION:
.SHELLFLAGS := -eu -o pipefail -c
.DELETE_ON_ERROR:
MAKEFLAGS += --warn-undefined-variables
MAKEFLAGS += --no-builtin-rules

ifdef V
Q=
WGET:=wget
else
Q=@
MAKEFLAGS += --no-print-directory
WGET:=wget -q --show-progress
endif

uniq = $(if $1,$(firstword $1) $(call uniq,$(filter-out $(firstword $1),$1)))

define QUIET
	$(if $(V), , $(1))
endef

MKDIR_P ?= mkdir -p
CP ?= cp -f
MV ?= mv 

.DEFAULT_GOAL := all


BUILD_DIR ?= build.nosync


.PHONY: all
all: web

layout.wasm: src/main.c $(wildcard src/*.h) | $(BUILD_DIR)
	$(Q)echo "Building Layout headless WASM (imported memory)..."
	$(Q)zig build-exe $< \
		-target wasm32-freestanding \
		-mcpu generic+atomics+bulk_memory \
		-fno-entry \
		-rdynamic \
		-O ReleaseSmall \
		-fstrip \
		--import-memory \
		--shared-memory \
		--initial-memory=18874368 \
		--max-memory=33554432 \
		-femit-bin=$@
	$(Q)echo "Exports: $$(wasm2wat $@ 2>/dev/null | grep -c '(export') functions"
	$(Q)ls -lh $@

$(BUILD_DIR):
	$(Q)$(MKDIR_P) $@

$(BUILD_DIR)/web:
	$(Q)$(MKDIR_P) $@

# Production web deployment target
.PHONY: web
web: layout.wasm | $(BUILD_DIR)
	$(Q)echo "Creating production web build in $(BUILD_DIR)/web/..."
	$(Q)$(MKDIR_P) $(BUILD_DIR)/web
	$(Q)$(MV) $< $(BUILD_DIR)/web/
	$(Q)$(CP) *.js $(BUILD_DIR)/web/
	$(Q)$(CP) *.html $(BUILD_DIR)/web/

.PHONY: clean
clean:
	$(Q)rm -rf $(BUILD_DIR)
