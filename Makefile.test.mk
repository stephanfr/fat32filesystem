# Copyright 2026 Stephan Friedl. All rights reserved.
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

include Makefile.toolchain.native.mk

ifneq ($(wildcard /opt/cpputest/include/CppUTest/TestHarness.h),)
CPPUTEST_PATH := /opt/cpputest
endif

MINIMALCLIB_DIR ?= ../minimalclib
MINIMALSTDIO_DIR ?= ../minimalstdio
MINIMALSTDLIB_DIR ?= ../minimalstdlib
BAREMETALBASE_DIR ?= ../baremetalbase
RPIBAREMETALOS_DIR ?= ../../rpibaremetalos

SRC_ROOT := src
SUPPORT_SRC_ROOT := $(RPIBAREMETALOS_DIR)/src/c
CPP_TEST_SRC_ROOT := test/src
TEST_BUILD_ROOT := test/build
OBJ_DIR := $(TEST_BUILD_ROOT)
TEST_OBJ_DIR := $(TEST_BUILD_ROOT)

CPP_TEST_SRC_DIRS := test/src test/src/filesystem test/src/filesystem/fat32_filesystem test/src/utility

DEP_CPP_SRC := $(wildcard $(SRC_ROOT)/*.cpp)
SUPPORT_CPP_SRC := $(SUPPORT_SRC_ROOT)/platform/platform_sw_rngs.cpp \
			   $(SUPPORT_SRC_ROOT)/services/os_entity_registry.cpp \
			   $(SUPPORT_SRC_ROOT)/services/murmur_hash.cpp \
			   $(SUPPORT_SRC_ROOT)/services/uuid.cpp

CPP_SRC := $(DEP_CPP_SRC) $(SUPPORT_CPP_SRC)
CPP_TEST_SRC := $(foreach sdir,$(CPP_TEST_SRC_DIRS),$(wildcard $(sdir)/*.cpp))

DEP_OBJ := $(patsubst $(SRC_ROOT)/%.cpp,$(TEST_BUILD_ROOT)/src/%.o,$(DEP_CPP_SRC))
SUPPORT_OBJ := $(patsubst $(SUPPORT_SRC_ROOT)/%.cpp,$(TEST_BUILD_ROOT)/support/%.o,$(SUPPORT_CPP_SRC))
OBJ := $(DEP_OBJ) $(SUPPORT_OBJ)
TEST_OBJ := $(patsubst $(CPP_TEST_SRC_ROOT)/%.cpp,$(TEST_BUILD_ROOT)/%.o,$(CPP_TEST_SRC))

TEST_BUILD_DIRS := $(sort $(dir $(OBJ)) $(dir $(TEST_OBJ)))

TEST_EXE := $(TEST_OBJ_DIR)/cpputest_main.exe

INCLUDE_DIRS := -Iinclude -I$(RPIBAREMETALOS_DIR)/include -I$(BAREMETALBASE_DIR)/include -I$(MINIMALSTDIO_DIR)/include -I$(MINIMALCLIB_DIR)/include -I$(MINIMALSTDLIB_DIR)/include $(INCLUDE_DIRS) -I$(CPPUTEST_PATH)/include
LDFLAGS += -L$(MINIMALCLIB_DIR)/lib/$(NATIVE_BUILD_DIR) -L$(MINIMALSTDIO_DIR)/lib/$(NATIVE_BUILD_DIR) -L$(MINIMALSTDLIB_DIR)/lib/$(NATIVE_BUILD_DIR) -L$(CPPUTEST_PATH)/lib
LDLIBS = -lCppUTest -lCppUTestExt -lminimalclib -lminimalstdio -lminimalstdlib

CDEFINES += -D__NO_LOGGING__

test: clean checkdirs $(TEST_EXE)

$(TEST_EXE): $(TEST_OBJ) $(OBJ)
	$(LD) $(OBJ) $(TEST_OBJ) $(LDFLAGS) $(LDLIBS) $(TEST_LIB) -o $(TEST_EXE)
	-./$(TEST_EXE)

$(TEST_BUILD_ROOT)/src/%.o: $(SRC_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(CDEFINES) -c $< -o $@

$(TEST_BUILD_ROOT)/support/%.o: $(SUPPORT_SRC_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(CDEFINES) -c $< -o $@

$(TEST_BUILD_ROOT)/%.o: $(CPP_TEST_SRC_ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDE_DIRS) $(CPP_FLAGS) $(TEST_CPP_FLAGS) $(TEST_OPTIMIZATION_FLAGS) $(CDEFINES) -c $< -o $@

checkdirs: $(TEST_BUILD_DIRS)

$(TEST_BUILD_DIRS):
	@mkdir -p $@

clean:
	@rm -rf $(TEST_BUILD_ROOT)

echo:
	@echo "CPP Sources:            " $(CPP_SRC)
	@echo "CPP Test Sources:       " $(CPP_TEST_SRC)
	@echo "Object Files:           " $(OBJ)
	@echo "CPP Test Object Files:  " $(TEST_OBJ)
