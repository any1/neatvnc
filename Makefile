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

DSO_NAME=libneatvnc
DSO_MAJOR=0
DSO_MINOR=0

DEPENDENCIES := pixman-1 libuv

SOURCES := \
	src/server.c \
	src/util.c \
	src/vec.c \
	src/zrle.c \

OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o) $(BUILD_DIR)/miniz.o

CFLAGS += -std=gnu11 -D_GNU_SOURCE -Iinc -fvisibility=hidden -Icontrib/miniz \
	  $(foreach dep,$(DEPENDENCIES),$(shell pkg-config --cflags $(dep)))

LDFLAGS += $(foreach dep,$(DEPENDENCIES),$(shell pkg-config --libs $(dep)))

DSO_PATH := $(BUILD_DIR)/$(DSO_NAME)

all: $(DSO_PATH).so.$(DSO_MAJOR).$(DSO_MINOR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DSO_PATH).so.$(DSO_MAJOR).$(DSO_MINOR): $(OBJECTS)
	$(CC) -fPIC -shared $^ $(LDFLAGS) -o $@
	ln -sf $(DSO_NAME).so.$(DSO_MAJOR).$(DSO_MINOR) $(DSO_PATH).so.$(DSO_MINOR)
	ln -sf $(DSO_NAME).so.$(DSO_MAJOR).$(DSO_MINOR) $(DSO_PATH).so

CC_OBJ = $(CC) -c $(CFLAGS) $< -o $@ $(CC_DEP_ARGS) -MMD -MP -MF $(@:.o=.deps)
$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR) ; $(CC_OBJ)
$(BUILD_DIR)/miniz.o: contrib/miniz/miniz.c | $(BUILD_DIR) ; $(CC_OBJ)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

-include $(BUILD_DIR)/*.deps

.SUFFIXES:
.SECONDARY:
