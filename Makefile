CC=gcc

C_DEBUG_FLAGS = -ggdb -DDEBUG_BUILD

LIBS = -lxcb -lxcb-icccm -lxcb-util -lxcb-xfixes

xcsyncd: main.c
	gcc -o $@ ${LIBS} $<

xcsyncd_debug: main.c
	gcc -o $@ ${LIBS} ${C_DEBUG_FLAGS} $<
