export PKG_CONFIG_PATH="/usr/local/opt/ncurses/lib/pkgconfig" # MacOS Homebrew

CCFLAGS := -std=c2x -pedantic \
           -Wall -Wextra -Wswitch-enum -Wimplicit-fallthrough \
           -Wno-unused-parameter -Wno-unused-variable -Wno-empty-translation-unit \
           $(shell pkg-config --cflags ncurses)

LDFLAGS := $(shell pkg-config --libs ncurses)

PRODUCT    := zep
OBJECT_EXT := .o
IMPL_EXT   := .c

$(PRODUCT): $(PRODUCT)$(OBJECT_EXT)
	cc $(LDFLAGS) -o $@ $^

%$(OBJECT_EXT): %$(IMPL_EXT)
	cc $(CCFLAGS) $(CCFLAGS_EDITOR_ADDITIONS) -c -o $@ $<

# ------------------------------------------------------------

.PHONY: clean

clean:
	rm $(PRODUCT) *$(OBJECT_EXT)
