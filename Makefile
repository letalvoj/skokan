# mluvitko / SKOKAN
#
#   make play       build and launch the macOS app (SPACE to jump)
#   make app        build SKOKAN.app only
#   make web        build the WebAssembly version into docs/ (GitHub Pages)
#   make icon       regenerate the app icon
#   make stats      measured bot rollouts, per-level difficulty table
#   make check      fairness contract: no unclearable configurations
#   make shots      render screenshots to host/shots/
#   make firmware   build every PlatformIO environment
#   make flash      flash the game to a connected board (auto-detects the port)
#   make monitor    watch the board's serial output
#   make clean      remove build output
#
# The macOS app and the firmware compile the SAME src/game.cpp; host/ only
# supplies what the ESP32 normally would (screen, clock, button, audio).

PIO      := $(HOME)/.local/bin/pio
# The library path contains spaces, so it is resolved by the shell and quoted
# at every use rather than living in a make variable that would get split.
GFX      := $(shell if [ -d ".pio/libdeps/game/Adafruit GFX Library" ]; \
              then echo ".pio/libdeps/game/Adafruit GFX Library"; \
              else echo ".pio/libdeps/mluvitko/Adafruit GFX Library"; fi)
APP      := host/SKOKAN.app
APPBIN   := $(APP)/Contents/MacOS/SKOKAN
ICNS     := $(APP)/Contents/Resources/AppIcon.icns
GAME_SRC := src/game.cpp host/panel.cpp host/display_host.cpp
CXXFLAGS := -std=c++17 -O2 -DGAME_HOST -DARDUINO=200 -Ihost/shim -Isrc \
            -Wno-unused-value -Wno-narrowing
SDL      := $(shell sdl2-config --cflags 2>/dev/null)
SDLLIBS  := $(shell sdl2-config --libs 2>/dev/null)

.PHONY: play app web icon stats check shots firmware flash monitor clean help
.DEFAULT_GOAL := help

help:
	@sed -n 's/^#   //p' $(firstword $(MAKEFILE_LIST))

# ---------------------------------------------------------------- macOS app
play: app
	@open $(APP)

app: $(APPBIN) $(ICNS) $(APP)/Contents/Info.plist
	@echo "built $(APP)"

$(APPBIN): $(GAME_SRC) host/play.cpp host/synth_sdl.cpp $(wildcard host/shim/*.h) src/synth.h src/display.h
	@command -v sdl2-config >/dev/null || { echo "SDL2 not found: brew install sdl2" >&2; exit 1; }
	@mkdir -p $(dir $@)
	clang++ $(CXXFLAGS) -I"$(GFX)" $(SDL) $(GAME_SRC) host/play.cpp host/synth_sdl.cpp \
	  "$(GFX)/Adafruit_GFX.cpp" $(SDLLIBS) -o $@

$(APP)/Contents/Info.plist: host/Info.plist
	@mkdir -p $(dir $@)
	@cp $< $@
	@printf 'APPL????' > $(APP)/Contents/PkgInfo

# The icon is rendered by a program, not cropped from a screenshot: it has to
# read at 32 px, where the game's HUD and text are just mud.
icon: $(ICNS)

$(ICNS): host/icon.cpp
	@mkdir -p host/icon $(dir $@)
	@clang++ -std=c++17 -O2 host/icon.cpp -lz -o host/icon/mkicon && ./host/icon/mkicon
	
	@rm -rf host/icon/AppIcon.iconset && mkdir -p host/icon/AppIcon.iconset
	@for s in 16 32 128 256 512; do \
	  sips -z $$s $$s host/icon/icon-1024.png --out host/icon/AppIcon.iconset/icon_$${s}x$${s}.png >/dev/null; \
	  d=$$((s * 2)); \
	  sips -z $$d $$d host/icon/icon-1024.png --out host/icon/AppIcon.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	@iconutil -c icns host/icon/AppIcon.iconset -o $@
	@echo "built $@"

# The same sources as the native app, with emscripten's SDL2 port. Output goes
# to docs/, which is what GitHub Pages serves at letalvoj.github.io/skokan.
web: docs/index.html

docs/index.html: $(GAME_SRC) host/play.cpp host/synth_sdl.cpp host/web/shell.html
	@command -v emcc >/dev/null || { echo "emscripten not found: brew install emscripten" >&2; exit 1; }
	@mkdir -p docs
	emcc $(CXXFLAGS) -I"$(GFX)" -s USE_SDL=2 -s ALLOW_MEMORY_GROWTH=1 -s ENVIRONMENT=web \
	  $(GAME_SRC) host/play.cpp host/synth_sdl.cpp "$(GFX)/Adafruit_GFX.cpp" \
	  --shell-file host/web/shell.html -o $@

# ------------------------------------------------------------- measurement
host/skokan: $(GAME_SRC) host/main_host.cpp host/synth_host.cpp $(wildcard host/shim/*.h)
	@./host/build.sh >/dev/null

stats: host/skokan
	@./host/skokan --stats 400

check: host/skokan
	@for s in 3 11 77 512; do printf "seed %-4s " $$s; \
	  ./host/skokan 12000 $$s 2>/dev/null | grep -oE "unclearable.*"; done

shots: host/skokan
	@./host/shots.sh

# ----------------------------------------------------------------- firmware
firmware:
	@$(PIO) run -e game -e game_usb -e mluvitko -e step1 -e step2 -e step3

# The board's two USB-C ports enumerate under different names and the name
# changes between sessions, so never pin one.
flash:
	@P=$$(ls /dev/cu.usbmodem* 2>/dev/null | head -1); \
	 [ -n "$$P" ] || { echo "no board found -- is it plugged in?" >&2; exit 1; }; \
	 echo "flashing via $$P"; $(PIO) run -e game_usb -t upload --upload-port $$P

monitor:
	@./tools/monitor.py 20 --no-reset

clean:
	rm -rf $(APP) host/skokan host/skokan-play host/icon/mkicon \
	       host/icon/*.bmp host/icon/*.png host/icon/AppIcon.iconset host/shots
