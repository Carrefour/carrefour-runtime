LIBGSL = $(shell gsl-config --libs)

CFLAGS = -Wall
LDLIBS = -lpthread -lnuma ${LIBGSL}

NB_NODES = $(shell lscpu | awk '/NUMA node\(s\)/ {print $$3}')
CFLAGS += -DNB_NODES=${NB_NODES}

.PHONY: all clean

all: makefile.dep carrefour-kthp tags

makefile.dep: *.[Cch]
	(for i in *.[Cc]; do ${CC} -MM "$${i}" ${CFLAGS}; done) > $@
   
-include makefile.dep

carrefour: carrefour.o

tags: *.[Cch]
	ctags --totals `find . -name '*.[ch]'`
	cscope -b -q -k -R -s.

clean_tags:
	rm cscope.*
	rm tags

clean: clean_tags
	rm -f carrefour-kthp *.o
