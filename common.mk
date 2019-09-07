MACHINE := $(shell $(CC) -dumpmachine)
ARCH := $(firstword $(subst -, ,$(MACHINE)))
BUILD_DIR ?= build-$(MACHINE)

ifeq ($(ARCH),x86_64)
	ARCH_CFLAGS := -mavx
else
ifeq ($(ARCH),arm)
	ARCH_CFLAGS := -mfpu=neon
endif # end arm block
endif # end x86_64 block

CFLAGS ?= -g -O3 $(ARCH_CFLAGS) -flto -DNDEBUG
LDFLAGS ?= -flto

CFLAGS += -std=gnu11 -D_GNU_SOURCE -Iinc 

CC_OBJ = $(CC) -c $(CFLAGS) $< -o $@ -MMD -MP -MF $(@:.o=.deps)
LINK_EXE = $(CC) $^ $(LDFLAGS) -o $@
LINK_DSO = $(CC) -fPIC -shared $^ $(LDFLAGS) -o $@

CFLAGS += $(foreach dep,$(DEPENDENCIES),$(shell pkg-config --cflags $(dep)))
LDFLAGS += $(foreach dep,$(DEPENDENCIES),$(shell pkg-config --libs $(dep)))
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)

$(BUILD_DIR): ; mkdir -p $(BUILD_DIR)

.PHONY: clean
clean: ; rm -rf $(BUILD_DIR)

-include $(BUILD_DIR)/*.deps

.SUFFIXES:
.SECONDARY:

# This clears the default target set by this file
.DEFAULT_GOAL :=
