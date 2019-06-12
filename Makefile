CC=gcc

C_DEBUG_FLAGS = -ggdb -DDEBUG_BUILD

LIBS = -lxcb -lxcb-icccm -lxcb-util -lxcb-xfixes

GGO_EXISTS := $(shell command -v gengetopt 2> /dev/null)

ifndef GGO_EXISTS
	$(error "There is no gengetopt on this system.")
endif

all: xcsyncd doc

cmdline.o: xcsyncd.ggo
	gengetopt --input=xcsyncd.ggo
	gcc -c -o cmdline.o cmdline.c

xcsyncd: cmdline.o main.c
	gcc -o $@ ${LIBS} $^

xcsyncd_debug: cmdline.o main.c
	gcc -o $@ ${LIBS} ${C_DEBUG_FLAGS} $^

doc: doxy.conf main.c
	doxygen doxy.conf

clean:
	rm --force *.o
	rm --force cmdline.c cmdline.h
	rm --force xcsyncd xcsyncd_debug
	rm --force --recursive doc

include test/Makefile.test

install: xcsyncd
	mkdir -p $(DESTDIR)/usr/bin/ $(DESTDIR)/usr/lib/systemd/user/
	cp xcsyncd $(DESTDIR)/usr/bin/xcsyncd
	cp xcsyncd.service $(DESTDIR)/usr/lib/systemd/user/
