all: libneatvnc.so

DEPENDENCIES := pixman-1 libpng libuv

CFLAGS := -g -O3 -mavx -DNDEBUG -std=gnu11 -D_GNU_SOURCE -Iinc \
	-fvisibility=hidden -Icontrib/miniz \
	$(foreach dep,$(DEPENDENCIES),$(shell pkg-config --cflags $(dep)))

LDFLAGS := $(foreach dep,$(DEPENDENCIES),$(shell pkg-config --libs $(dep)))

libneatvnc.so.0.0: src/server.o src/util.o src/vec.o src/zrle.o src/pngfb.o \
		src/miniz.o
	$(CC) -fPIC -shared $^ $(LDFLAGS) -o $@

libneatvnc.so.0: libneatvnc.so.0.0
	ln -sf $^ $@

libneatvnc.so: libneatvnc.so.0
	ln -sf $^ $@

zrle-bench: bench/zrle-bench.o src/server.o src/util.o src/vec.o src/zrle.o \
		src/pngfb.o src/miniz.o
	$(CC) $^ $(LDFLAGS) -o $@

examples/png-server: examples/png-server.o src/pngfb.o libneatvnc.so
	$(CC) $^ $(LDFLAGS) -L. -lneatvnc -Wl,-rpath=$(shell pwd) -o $@

examples/draw: examples/draw.o libneatvnc.so
	$(CC) $^ $(LDFLAGS) -L. -lneatvnc -Wl,-rpath=$(shell pwd) -o $@

src/%.o: src/%.c
	$(CC) -c $(CFLAGS) $< -o $@ -MMD -MP -MF $@.deps

src/miniz.o: contrib/miniz/miniz.c
	$(CC) -c $(CFLAGS) $< -o $@ -MMD -MP -MF $@.deps

bench/%.o: bench/%.c
	$(CC) -c $(CFLAGS) $< -o $@ -MMD -MP -MF $@.deps

examples/%.o: examples/%.c
	$(CC) -c $(CFLAGS) $< -o $@ -MMD -MP -MF $@.deps

.PHONY: clean
clean:
	rm -f libneatvnc.so*
	rm -f src/*.o src/*.deps bench/*.o bench/*.deps

-include src/*.deps

.SUFFIXES:
.SECONDARY:
