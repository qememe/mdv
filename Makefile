CC = cc
CPPFLAGS += -Ivendor/md4c
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
SOURCES = src/mdv.c vendor/md4c/md4c.c vendor/md4c/entity.c

.PHONY: all clean install uninstall test
all: mdv

mdv: $(SOURCES) vendor/md4c/md4c.h vendor/md4c/entity.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

install: mdv
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 mdv "$(DESTDIR)$(BINDIR)/mdv"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/mdv"

test: mdv
	sh tests/test_cli.sh ./mdv

clean:
	rm -f mdv
