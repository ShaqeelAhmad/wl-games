HOTRELOAD = 0

WL_SCANNER = wayland-scanner
WL_PROTOCOLS_DIR = /usr/share/wayland-protocols/
CFLAGS += `pkg-config --cflags cairo wayland-client`
LDFLAGS += -lrt -lm `pkg-config --libs cairo wayland-client wayland-cursor xkbcommon`

XDG_SHELL = $(WL_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
XDG_DECORATION = $(WL_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml

WL_SRC = xdg-shell-protocol.c xdg-decoration-unstable-protocol.c
WL_HDR = xdg-shell-client-protocol.h xdg-decoration-unstable-client-protocol.h

CFLAGS += -g3 -ggdb -std=c11 -pedantic -Wall -Wextra -Wno-unused-parameter
CFLAGS += -I . -D_POSIX_C_SOURCE=200809L

CFLAGS += -DHOTRELOAD=$(HOTRELOAD)

# CFLAGS += -fsanitize=address,undefined

GAMES_1 =
GAMES_0 = games.c
LIBGAMES_1 = libgames.so
LIBGAMES_0 =
LIBGAMES_LDFLAGS = `pkg-config --libs cairo`
SRC = main.c shm.c $(GAMES_$(HOTRELOAD)) $(WL_SRC)

all: wl-games

libgames.so: games.c
	$(CC) $(CFLAGS) -shared -fPIC $< -o $@ $(LIBGAMES_LDFLAGS)

wl-games: $(SRC) $(WL_HDR) $(LIBGAMES_$(HOTRELOAD))
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRC) -o $@

xdg-shell-protocol.c:
	$(WL_SCANNER) private-code $(XDG_SHELL) $@

xdg-shell-client-protocol.h:
	$(WL_SCANNER) client-header $(XDG_SHELL) $@

xdg-decoration-unstable-protocol.c:
	$(WL_SCANNER) private-code $(XDG_DECORATION) $@

xdg-decoration-unstable-client-protocol.h:
	$(WL_SCANNER) client-header $(XDG_DECORATION) $@

clean:
	rm -f wl-games *-protocol.c *-protocol.h libgames.so

.PHONY: clean
