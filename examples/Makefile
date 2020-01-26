DEPENDENCIES := libpng pixman-1 libuv

include ../common.mk

ifdef LIB_PATH
LDFLAGS += -L$(LIB_PATH) -Wl,-rpath=$(shell pwd)/$(LIB_PATH)
endif

LDFLAGS += -lneatvnc

all: \
	$(BUILD_DIR)/png-server \
	$(BUILD_DIR)/draw \

$(BUILD_DIR)/png-server: $(BUILD_DIR)/png-server.o $(BUILD_DIR)/pngfb.o
	$(LINK_EXE)

$(BUILD_DIR)/draw: $(BUILD_DIR)/draw.o ; $(LINK_EXE)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR) ; $(CC_OBJ)

$(BUILD_DIR)/pngfb.o: ../src/pngfb.c | $(BUILD_DIR)
	$(CC_OBJ) -I../include

