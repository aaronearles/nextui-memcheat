CC      ?= aarch64-linux-musl-gcc
CFLAGS  = -std=c99 -O2 -Wall -Wextra -static -pthread -D_GNU_SOURCE
LDFLAGS = -static -pthread

SRCS    = src/main.c src/scanner.c src/watch.c src/procutil.c \
          src/cht.c src/api.c src/mongoose.c
TARGET  = bin/aarch64/memcheat

MONGOOSE_VERSION ?= 7.21
PAKMAN_VERSION   ?= 0.24.17
PAKMAN_ZIP       = /tmp/Pakman-nextui.zip
PAKMAN_URL       = https://github.com/josegonzalez/pakman/releases/download/$(PAKMAN_VERSION)/Pakman-nextui.zip

.PHONY: all daemon tools package clean

all: daemon tools package

# ── daemon ────────────────────────────────────────────────────────────────
daemon: src/mongoose.c src/mongoose.h $(TARGET)

$(TARGET): $(SRCS) src/scanner.h src/watch.h src/procutil.h src/cht.h src/api.h src/state.h
	mkdir -p bin/aarch64
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

src/mongoose.h:
	curl -fsSL -o $@ \
	  "https://raw.githubusercontent.com/cesanta/mongoose/$(MONGOOSE_VERSION)/mongoose.h"

src/mongoose.c: src/mongoose.h
	curl -fsSL -o $@ \
	  "https://raw.githubusercontent.com/cesanta/mongoose/$(MONGOOSE_VERSION)/mongoose.c"

# ── tool binaries ─────────────────────────────────────────────────────────
tools: bin/tg5040/minui-list bin/tg5040/minui-presenter bin/tg5040/jq

$(PAKMAN_ZIP):
	curl -fsSL -o $@ "$(PAKMAN_URL)"

bin/tg5040/minui-list: $(PAKMAN_ZIP)
	mkdir -p bin/tg5040
	unzip -j -o $< \
	  'Pakman/Tools/tg5040/SSH Server.pak/bin/tg5040/minui-list' \
	  -d bin/tg5040/
	chmod +x $@

bin/tg5040/minui-presenter: $(PAKMAN_ZIP)
	mkdir -p bin/tg5040
	unzip -j -o $< \
	  'Pakman/Tools/tg5040/SSH Server.pak/bin/tg5040/minui-presenter' \
	  -d bin/tg5040/
	chmod +x $@

bin/tg5040/jq: $(PAKMAN_ZIP)
	mkdir -p bin/tg5040
	unzip -j -o $< \
	  'Pakman/Tools/tg5040/Search.pak/bin/arm64/jq' \
	  -d bin/tg5040/
	chmod +x $@

# ── package ───────────────────────────────────────────────────────────────
package: daemon tools
	mkdir -p dist
	zip -r "dist/Memory-Editor.pak.zip" \
	  pak.json settings.json launch.sh www/ \
	  bin/shared/ \
	  bin/aarch64/memcheat \
	  bin/tg5040/minui-list \
	  bin/tg5040/minui-presenter \
	  bin/tg5040/jq

# ── clean ─────────────────────────────────────────────────────────────────
clean:
	rm -f $(TARGET)
	rm -rf dist/
