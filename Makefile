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
DSO := $(DSO_PATH).so.$(DSO_MAJOR).$(DSO_MINOR)

$(DSO): $(OBJECTS)
	$(LINK_DSO)
	ln -sf $(DSO_NAME).so.$(DSO_MAJOR).$(DSO_MINOR) $(DSO_PATH).so.$(DSO_MINOR)
	ln -sf $(DSO_NAME).so.$(DSO_MAJOR).$(DSO_MINOR) $(DSO_PATH).so

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR) ; $(CC_OBJ)
$(BUILD_DIR)/miniz.o: contrib/miniz/miniz.c | $(BUILD_DIR) ; $(CC_OBJ)

BENCH_DIR = $(BUILD_DIR)/bench

$(BENCH_DIR)/%.o: bench/%.c | $(BENCH_DIR)
	$(CC_OBJ) $(shell pkg-config --cflags libpng)

$(BENCH_DIR): ; mkdir -p $@
$(BENCH_DIR)/zrle-bench:
$(BENCH_DIR)/zrle-bench: $(OBJECTS) $(BUILD_DIR)/pngfb.o \
		$(BENCH_DIR)/zrle-bench.o
	$(LINK_EXE) $(shell pkg-config --libs libpng)

.PHONY: bench
bench: $(BENCH_DIR)/zrle-bench
	./$(BENCH_DIR)/zrle-bench

.PHONY: examples
examples: $(DSO)
	make -C examples \
		BUILD_DIR=../$(BUILD_DIR)/examples \
		LIB_PATH=../$(BUILD_DIR)
