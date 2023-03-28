CFLAGS:=-std=gnu99 -O2 -pedantic -Wall -Wextra -Wwrite-strings -Warray-bounds -Wconversion -Wstrict-prototypes -Werror $(CFLAGS)
CPPFLAGS:=$(CPPFLAGS)

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

INSTALL:=install
prefix:=/usr/local
bindir:=$(prefix)/bin
datarootdir:=$(prefix)/share
mandir:=$(datarootdir)/man

all: $(EXECUTABLES)

%: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@ $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -c -o $@

%: %.o
	$(CC) $< -o $@ $(LIBS) $(LDFLAGS)

# Noisy clang build that's expected to fail, but can be useful to find corner
# cases.
clang-everything: CC=clang
clang-everything: CFLAGS+=-Weverything -Wno-disabled-macro-expansion -Wno-padded -Wno-covered-switch-default -Wno-gnu-zero-variadic-macro-arguments
clang-everything: all

sanitisers: CFLAGS+=-fsanitize=address -fsanitize=undefined -fanalyzer
sanitisers: debug

debug: CFLAGS+=-Og -ggdb -fno-omit-frame-pointer
debug: all

clang-tidy:
	# DeprecatedOrUnsafeBufferHandling: See https://stackoverflow.com/a/50724865/945780
	# clang-diagnostic-gnu-zero-variadic-macro-arguments: We require this for ##__VA_ARGS__
	clang-tidy zcfan.c -checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-clang-diagnostic-gnu-zero-variadic-macro-arguments -- $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

install: all
	mkdir -p $(DESTDIR)$(bindir)/
	$(INSTALL) -pt $(DESTDIR)$(bindir)/ $(EXECUTABLES)
	$(INSTALL) -Dp -m 644 zcfan.service $(DESTDIR)$(prefix)/lib/systemd/system/zcfan.service
	$(INSTALL) -Dp -m 644 zcfan.1 $(DESTDIR)$(mandir)/man1/zcfan.1

lint:
	clang-format -style=file --dry-run --Werror zcfan.c

clean:
	rm -f $(EXECUTABLES)
