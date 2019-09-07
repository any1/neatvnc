DEPENDENCIES := pixman-1 libuv

SOURCES := \
	src/server.c \
	src/util.c \
	src/vec.c \
	src/zrle.c \

include common.mk

DSO_NAME=libneatvnc
DSO_MAJOR=0
DSO_MINOR=0

CFLAGS += -fvisibility=hidden -Icontrib/miniz
OBJECTS += $(BUILD_DIR)/miniz.o

DSO_PATH := $(BUILD_DIR)/$(DSO_NAME)

$(DSO_PATH).so.$(DSO_MAJOR).$(DSO_MINOR): $(OBJECTS)
	$(LINK_DSO)
	ln -sf $(DSO_NAME).so.$(DSO_MAJOR).$(DSO_MINOR) $(DSO_PATH).so.$(DSO_MINOR)
	ln -sf $(DSO_NAME).so.$(DSO_MAJOR).$(DSO_MINOR) $(DSO_PATH).so

$(BUILD_DIR)/miniz.o: contrib/miniz/miniz.c | $(BUILD_DIR) ; $(CC_OBJ)
