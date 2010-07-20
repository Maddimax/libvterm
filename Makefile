CCFLAGS=-Wall -Iinclude -std=c99
LDFLAGS=-lutil

ifeq ($(DEBUG),1)
  CCFLAGS+=-ggdb -DDEBUG
endif

ifeq ($(PROFILE),1)
  CCFLAGS+=-pg
  LDFLAGS+=-pg
endif

CCFLAGS+=$(shell pkg-config --cflags glib-2.0)
LDFLAGS+=$(shell pkg-config --libs   glib-2.0)

CFILES=$(wildcard src/*.c)
OFILES=$(CFILES:.c=.o)
HFILES=$(wildcard include/*.h)

HFILES_INT=$(wildcard src/*.h) $(HFILES)

TEST_CFILES=$(wildcard t/*.c)
TEST_OFILES=$(TEST_CFILES:.c=.o)

LIBPIECES=vterm parser encoding state input pen unicode

all: pangoterm

pangoterm: pangoterm.c libvterm.so
	gcc -o $@ $^ $(CCFLAGS) $(shell pkg-config --cflags --libs gtk+-2.0) $(LDFLAGS)

libvterm.so: $(addprefix src/, $(addsuffix .o, $(LIBPIECES)))
	gcc -shared -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c $(HFILES_INT)
	gcc -fPIC -o $@ -c $< $(CCFLAGS)

# Need first to cancel the implict rule
%.o: %.c

t/%.o: t/%.c t/%.inc $(HFILES)
	gcc -c -o $@ $< $(CCFLAGS)

t/%.inc: t/%.c
	t/gen-test.inc.sh $< >$@

t/test.o: t/test.c t/extern.h t/suites.h
	gcc -c -o $@ $< $(CCFLAGS)

t/extern.h: t
	t/test.c.sh

t/test: libvterm.so $(TEST_OFILES)
	t/test.c.sh
	gcc -o $@ $^ $(CCFLAGS) $(LDFLAGS) -lcunit

.PHONY: test
test: libvterm.so t/test
	LD_LIBRARY_PATH=. t/test

.PHONY: clean
clean:
	rm -f $(DEBUGS) $(OFILES) $(TEST_OFILES) $(TEST_CFILES:.c=.inc) libvterm.so
