# Copyright 2026 Stephan Friedl. All rights reserved.
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

NATIVE_TARGETS := native

ifneq (,$(filter $(NATIVE_TARGETS), $(MAKECMDGOALS)))
include Makefile.native.mk
else
include Makefile.aarch64.mk
endif


.PHONY: test coverage asan
test:
	@$(MAKE) -f Makefile.test.mk test

coverage:
	@$(MAKE) -f Makefile.test.mk test \
	    TEST_CFLAGS="$(COVERAGE_CFLAGS)" TEST_OPTIMIZATION_FLAGS="$(COVERAGE_OPTIMIZATION_FLAGS)"
	@gcov -r test/build/*.gcda | tail -40

asan:
	@$(MAKE) -f Makefile.test.mk test \
	    TEST_OPTIMIZATION_FLAGS="-O1" \
	    TEST_CFLAGS="$(TEST_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer"
