# Copyright 2026 Stephan Friedl. All rights reserved.
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

NATIVE_TARGETS := native

ifneq (,$(filter $(NATIVE_TARGETS), $(MAKECMDGOALS)))
include Makefile.native.mk
else
include Makefile.aarch64.mk
endif
