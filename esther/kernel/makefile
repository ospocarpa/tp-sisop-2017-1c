CC=gcc
MAKE=make

LIBS = -lcommons -lqepd -lparser-ansisop
LIBPATH=../QEPDLibrary
LIBBINPATH=$(LIBPATH)/build

SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)

all: kernel

kernel: $(SRCS) $(LIBPATH)/build/libqepd.so
	$(CC) -o $@ $^ -I$(LIBPATH) -L$(LIBBINPATH) $(LIBS) -Wl,-rpath=$(LIBBINPATH)

$(LIBPATH)/build/libqepd.so:
	cd $(LIBPATH); $(MAKE)

debug: CC += -DDEBUG -g
debug: MAKE += debug
debug: all

clean:
	rm -f kernel
	rm -f kernel_*.log

.PHONY: compile clean
