all: neatvnc

DEPENDENCIES := pixman-1 libpng libuv

CFLAGS := -g -O0 -std=gnu11 -D_GNU_SOURCE -Iinc \
	$(foreach dep,$(DEPENDENCIES),$(shell pkg-config --cflags $(dep)))

LDFLAGS := $(foreach dep,$(DEPENDENCIES),$(shell pkg-config --libs $(dep)))

neatvnc: src/server.o src/util.o src/vec.o src/zrle.o src/pngfb.o
	$(CC) $^ $(LDFLAGS) -o $@

src/%.o: src/%.c
	$(CC) -c $(CFLAGS) $< -o $@ -MMD -MP -MF $@.deps

.PHONY: clean
clean:
	rm -f neatvnc
	rm -f src/*.o src/*.deps

-include src/*.deps

.SUFFIXES:
.SECONDARY:
