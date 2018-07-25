CC=gcc

C_DEBUG_FLAGS = -ggdb -DDEBUG_BUILD

LIBS = -lxcb -lxcb-icccm -lxcb-util -lxcb-xfixes

GGO_EXISTS := $(shell command -v gengetopt 2> /dev/null)

check_gengetopt:
ifndef GGO_EXISTS
	$(error "There is no gengetopt on this system.")
endif

cmdline.o: xcsyncd.ggo check_gengetopt
	gengetopt --input=xcsyncd.ggo
	gcc -c -o cmdline.o cmdline.c

xcsyncd: cmdline.o main.c
	gcc -o $@ ${LIBS} $^

xcsyncd_debug: cmdline.o main.c
	gcc -o $@ ${LIBS} ${C_DEBUG_FLAGS} $^

clean:
	rm -rf *.o
	rm xcsyncd xcsyncd_debug

include test/Makefile.test

install: xcsyncd
	mkdir -p $(DESTDIR)/usr/bin/
	cp xcsyncd $(DESTDIR)/usr/bin/xcsyncd
