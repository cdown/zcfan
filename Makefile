CFLAGS:=-std=gnu99 -O2 -pedantic -Wall -Wextra -Werror $(CFLAGS)

SOURCES=$(wildcard *.c)
EXECUTABLES=$(patsubst %.c,%,$(SOURCES))

all: $(EXECUTABLES)

%: %.c
	$(CC) $(CFLAGS) $< -o $@ $(LIBS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) $< -c -o $@

%: %.o
	$(CC) $< -o $@ $(LIBS) $(LDFLAGS)

# Noisy clang build that's expected to fail, but can be useful to find corner
# cases.
clang-everything: CC=clang
clang-everything: CFLAGS+=-Weverything -Wno-disabled-macro-expansion -Wno-padded -Wno-covered-switch-default
clang-everything: all

sanitisers: CFLAGS+=-fsanitize=address -fsanitize=undefined
sanitisers: debug

debug: CFLAGS+=-Og -ggdb -fno-omit-frame-pointer
debug: all

clang-tidy:
	# DeprecatedOrUnsafeBufferHandling: See https://stackoverflow.com/a/50724865/945780
	clang-tidy zcfan.c -checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling -- $(CFLAGS) $(LDFLAGS)

clean:
	rm -f $(EXECUTABLES)
