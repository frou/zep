# On macOS, find a ncurses installed using Homebrew (will be newer than the system one)
PKGCONFIG_TWEAK := PKG_CONFIG_PATH=/usr/local/opt/ncurses/lib/pkgconfig

CCFLAGS := -std=c99 -pedantic \
           -Wall -Wextra -Wswitch-enum -Wimplicit-fallthrough \
           -Wno-unused-parameter -Wno-unused-variable -Wno-empty-translation-unit \
           $(shell $(PKGCONFIG_TWEAK) pkg-config --cflags ncurses)

LDFLAGS := $(shell $(PKGCONFIG_TWEAK) pkg-config --libs ncurses)

PRODUCT    := zep
OBJECT_EXT := .o
IMPL_EXT   := .c

# REF: https://clangd.llvm.org/installation.html#project-setup
LSP_HINT_FILE := compile_flags.txt

$(PRODUCT): $(PRODUCT)$(OBJECT_EXT)
	@ printf "%s\n" $(CCFLAGS) >$(LSP_HINT_FILE)
	cc $(LDFLAGS) -o $@ $^

%$(OBJECT_EXT): %$(IMPL_EXT)
	cc $(CCFLAGS) $(CCFLAGS_EDITOR_ADDITIONS) -c -o $@ $^

# ------------------------------------------------------------

.PHONY: clean

clean:
	rm $(PRODUCT) *$(OBJECT_EXT)
