DEPENDENCIES := pixman-1 libuv libturbojpeg

SOURCES := \
	src/server.c \
	src/util.c \
	src/vec.c \
	src/zrle.c \
	src/tight.c \
	src/raw-encoding.c \
	src/pixels.c \
	src/damage.c \
	src/fb.c \

include common.mk

VERSION=0.0.0

DSO_NAME=libneatvnc
DSO_MAJOR=0
DSO_MINOR=0

CFLAGS += -fvisibility=hidden -Icontrib/miniz
OBJECTS += $(BUILD_DIR)/miniz.o

DSO_PATH := $(BUILD_DIR)/$(DSO_NAME)
DSO := $(DSO_PATH).so.$(DSO_MAJOR).$(DSO_MINOR)

ifndef DONT_STRIP
	INSTALL_STRIP := -s --strip-program=$(STRIP)
endif

$(DSO): $(OBJECTS)
	$(LINK_DSO)
	ln -sf $(DSO_NAME).so.$(DSO_MAJOR).$(DSO_MINOR) $(DSO_PATH).so.$(DSO_MINOR)
	ln -sf $(DSO_NAME).so.$(DSO_MAJOR).$(DSO_MINOR) $(DSO_PATH).so

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR) ; $(CC_OBJ)
$(BUILD_DIR)/miniz.o: contrib/miniz/miniz.c | $(BUILD_DIR) ; $(CC_OBJ)

$(BUILD_DIR)/neatvnc.pc:
	PREFIX=$(PREFIX) VERSION=$(VERSION) ./gen-pkgconfig.sh >$@

BENCH_DIR = $(BUILD_DIR)/bench

$(BENCH_DIR)/%.o: bench/%.c | $(BENCH_DIR)
	$(CC_OBJ) $(shell $(PKGCONFIG) --cflags libpng)

$(BENCH_DIR): ; mkdir -p $@
$(BENCH_DIR)/zrle-bench:
$(BENCH_DIR)/zrle-bench: $(OBJECTS) $(BUILD_DIR)/pngfb.o \
		$(BENCH_DIR)/zrle-bench.o
	$(LINK_EXE) $(shell $(PKGCONFIG) --libs libpng)

.PHONY: install
install: $(DSO) $(BUILD_DIR)/neatvnc.pc
	install $(INSTALL_STRIP) -Dt $(DESTDIR)$(PREFIX)/lib $(BUILD_DIR)/*.so*
	install -Dt $(DESTDIR)$(PREFIX)/lib/pkgconfig $(BUILD_DIR)/neatvnc.pc
	install -Dt $(DESTDIR)$(PREFIX)/include include/neatvnc.h

.PHONY: bench
bench: $(BENCH_DIR)/zrle-bench
	./$(BENCH_DIR)/zrle-bench

.PHONY: examples
examples: $(DSO)
	make -C examples \
		BUILD_DIR=../$(BUILD_DIR)/examples \
		LIB_PATH=../$(BUILD_DIR)
