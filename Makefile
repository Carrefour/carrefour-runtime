LIBGSL = $(shell gsl-config --libs)

CFLAGS = -Wall
LDLIBS = -lpthread -lnuma ${LIBGSL}

.PHONY: all clean

all: carrefour tags

carrefour: carrefour.c carrefour.h

tags: carrefour.c carrefour.h
	ctags --totals `find . -name '*.[ch]'`
	cscope -b -q -k -R -s.

clean_tags:
	rm cscope.*
	rm tags

clean: clean_tags
	rm -f carrefour *o
