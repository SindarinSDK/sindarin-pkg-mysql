# Sindarin MySQL Package

.PHONY: setup test build teardown

%.sn: %.sn.c
	@:

ifeq ($(OS),Windows_NT)
    PLATFORM := windows
    EXE_EXT  := .exe
    PLATFORM_LDFLAGS := -lshlwapi -lsecur32 -lcrypt32 -ladvapi32
else
    UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM := darwin
    else
        PLATFORM := linux
    endif
    EXE_EXT :=
    PLATFORM_LDFLAGS :=
endif

BIN_DIR      := bin
SN           ?= sn
SRC_SOURCES  := $(wildcard src/*.sn) $(wildcard src/*.sn.c)
RUN_TESTS_SN := .sn/sindarin-pkg-test/src/execute.sn
RUN_TESTS    := $(BIN_DIR)/run_tests$(EXE_EXT)

export MYSQL_HOST     ?= 127.0.0.1
export MYSQL_PORT     ?= 3306
export MYSQL_DATABASE ?= testdb
export MYSQL_USER     ?= testuser
export MYSQL_PASSWORD ?= testpass
export SN_CFLAGS      := -I$(CURDIR)/libs/$(PLATFORM)/include $(SN_CFLAGS)
export SN_LDFLAGS     := -L$(CURDIR)/libs/$(PLATFORM)/lib $(PLATFORM_LDFLAGS) $(SN_LDFLAGS)

setup:
	@$(SN) --install
ifeq ($(OS),Windows_NT)
	@powershell -ExecutionPolicy Bypass -File scripts/install.ps1
else
	@bash scripts/install.sh
endif
	@docker compose up -d --wait

test: setup $(RUN_TESTS)
	@$(RUN_TESTS) --verbose

teardown:
	@docker compose down

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(RUN_TESTS): $(RUN_TESTS_SN) $(SRC_SOURCES) | $(BIN_DIR)
	@$(SN) $(RUN_TESTS_SN) -o $@ -l 1

VCPKG_ROOT ?= $(CURDIR)/vcpkg
TRIPLET    ?= $(if $(filter windows,$(PLATFORM)),x64-mingw-static,$(if $(filter aarch64,$(shell uname -m 2>/dev/null)),arm64,x64)-$(if $(filter darwin,$(PLATFORM)),osx,linux))
ARCH       ?= $(if $(filter aarch64,$(shell uname -m 2>/dev/null)),arm64,x64)
VERSION    ?= local

build:
	@if [ ! -x "$(VCPKG_ROOT)/vcpkg" ] && [ ! -x "$(VCPKG_ROOT)/vcpkg.exe" ]; then \
	    echo "Bootstrapping vcpkg..." && \
	    git clone --depth=1 https://github.com/microsoft/vcpkg.git "$(VCPKG_ROOT)" && \
	    "$(VCPKG_ROOT)/bootstrap-vcpkg.sh" -disableMetrics; \
	fi
	"$(VCPKG_ROOT)/vcpkg" install --triplet=$(TRIPLET) --x-install-root=vcpkg/installed
	mkdir -p libs/$(PLATFORM)/lib libs/$(PLATFORM)/include
	find vcpkg/installed/$(TRIPLET)/lib -maxdepth 1 -name "*.a" -exec cp {} libs/$(PLATFORM)/lib/ \;
	cp -r vcpkg/installed/$(TRIPLET)/include/* libs/$(PLATFORM)/include/
	echo "$(VERSION)" > libs/$(PLATFORM)/VERSION
	echo "$(PLATFORM)" > libs/$(PLATFORM)/PLATFORM
