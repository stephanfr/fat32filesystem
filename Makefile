# Copyright 2026 Stephan Friedl. All rights reserved.
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

NATIVE_TARGETS := native

ifneq (,$(filter $(NATIVE_TARGETS), $(MAKECMDGOALS)))
include Makefile.native.mk
else
include Makefile.aarch64.mk
endif

#  The test, coverage and sanitizer builds are defined in Makefile.test.mk, which includes the
#      native toolchain.  This file includes the aarch64 toolchain, where none of the test or
#      coverage variables exist - so it must only delegate, never pass flags of its own.

.PHONY: test coverage asan
test coverage asan:
	@$(MAKE) -f Makefile.test.mk $@
