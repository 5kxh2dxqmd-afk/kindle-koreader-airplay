# Makefile — Build complete KOReader AirPlay plugin
#
# OUTPUT:  dist-$(TARGET)/airplay.koplugin/
#          Copy this directory to: <kindle>/koreader/plugins/
#
# TARGETS:
#   make kindle TARGET=sf|hf         Cross-compile for Kindle ARM  ← use this
#   make docker-kindle TARGET=sf|hf  Same but via Docker (no local toolchain needed)
#   make native                      Build for Mac host (testing only)
#   make clean
#
# TARGET SELECTION:
#   sf  (default) — soft-float ABI. Kindle firmware <=5.16.2.1.1
#                   (Paperwhite 1-5, Voyage, Oasis 1/2/3, older Scribe FW).
#   hf             — hard-float ABI. Kindle firmware >=5.16.3
#                   (Paperwhite 6/12 and any device updated past 5.16.2.1.1).
#   Amazon switched their build toolchain from soft- to hard-float at that
#   firmware boundary; the two are not binary compatible, hence two builds.
#
# SETUP (Mac):
#   Option A — prebuilt cross-compiler (fastest):
#     sf: brew tap messense/macos-cross-toolchains && \
#         brew install arm-unknown-linux-gnueabi
#     hf: download koreader/koxtoolchain's "kindlehf" release tarball and
#         extract to ~/x-tools, then add .../arm-kindlehf-linux-gnueabihf/bin
#         to PATH. https://github.com/koreader/koxtoolchain/releases
#
#   Option B — Docker (no toolchain install needed):
#     brew install --cask docker
#     make docker-kindle TARGET=hf
#
# OPTIONAL VARS:
#   FFMPEG_PREFIX=/path   custom ffmpeg prefix (headers + libs)
#   KOXTC=/path/bin       use koxtoolchain cross-compiler dir instead

# ── Paths ──────────────────────────────────────────────────────────
UXPLAY     := vendor/uxplay/lib

# UxPlay lib files — exclude dnssd.c (replaced by our Kindle stub)
UXPLAY_SRC := \
    $(UXPLAY)/raop.c \
    $(UXPLAY)/raop_rtp.c \
    $(UXPLAY)/raop_rtp_mirror.c \
    $(UXPLAY)/raop_buffer.c \
    $(UXPLAY)/raop_ntp.c \
    $(UXPLAY)/mirror_buffer.c \
    $(UXPLAY)/crypto.c \
    $(UXPLAY)/pairing.c \
    $(UXPLAY)/httpd.c \
    $(UXPLAY)/http_request.c \
    $(UXPLAY)/http_response.c \
    $(UXPLAY)/llhttp/llhttp.c \
    $(UXPLAY)/llhttp/api.c \
    $(UXPLAY)/llhttp/http.c \
    $(UXPLAY)/logger.c \
    $(UXPLAY)/byteutils.c \
    $(UXPLAY)/compat.c \
    $(UXPLAY)/netutils.c \
    $(UXPLAY)/utils.c \
    $(UXPLAY)/fairplay_playfair.c \
    $(UXPLAY)/playfair/playfair.c \
    $(UXPLAY)/playfair/sap_hash.c \
    $(UXPLAY)/playfair/omg_hax.c \
    $(UXPLAY)/playfair/hand_garble.c \
    $(UXPLAY)/playfair/modified_md5.c \
    $(UXPLAY)/srp.c \
    $(UXPLAY)/airplay_video.c

SRC_COMMON := src/airplay_wrapper.c src/dnssd_stub.c $(UXPLAY_SRC)
HDR        := src/airplay_mirror.h
PLUGIN_SRC := plugin/_meta.lua plugin/main.lua plugin/airplay_ffi.lua
LIB        := libairplay_mirror.so

# ── Target selection (sf | hf) ────────────────────────────────────
TARGET ?= sf

ifeq ($(TARGET),hf)
  # HF: koxtoolchain's "kindlehf" toolchain bundles a modern-enough glibc
  # already (it targets the actual on-device sysroot), so none of the
  # fcntl64/getentropy@GLIBC_2.17 shims in src/compat_glibc.c are needed
  # or wanted here — that file is SF-only.
  SRC        := $(SRC_COMMON)
  ARM_CFLAGS := -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -flax-vector-conversions

  ifdef KOXTC
    CC_KINDLE := $(KOXTC)/arm-kindlehf-linux-gnueabihf-gcc
    STRIP_K   := $(KOXTC)/arm-kindlehf-linux-gnueabihf-strip
  else
    _KOXTC_HF := $(shell command -v arm-kindlehf-linux-gnueabihf-gcc 2>/dev/null)
    _APT_HF   := $(shell command -v arm-linux-gnueabihf-gcc 2>/dev/null)
    ifneq ($(_KOXTC_HF),)
      CC_KINDLE := arm-kindlehf-linux-gnueabihf-gcc
      STRIP_K   := arm-kindlehf-linux-gnueabihf-strip
    else ifneq ($(_APT_HF),)
      CC_KINDLE := arm-linux-gnueabihf-gcc
      STRIP_K   := arm-linux-gnueabihf-strip
    else
      CC_KINDLE := arm-kindlehf-linux-gnueabihf-gcc   # will fail with useful error
      STRIP_K   := arm-kindlehf-linux-gnueabihf-strip
    endif
  endif
else ifeq ($(TARGET),sf)
  SRC        := $(SRC_COMMON) src/compat_glibc.c
  ARM_CFLAGS := -march=armv7-a -mfloat-abi=softfp -mfpu=neon -flax-vector-conversions

  ifdef KOXTC
    CC_KINDLE := $(KOXTC)/arm-unknown-linux-gnueabi-gcc
    STRIP_K   := $(KOXTC)/arm-unknown-linux-gnueabi-strip
  else
    _MESSENSE := $(shell command -v arm-unknown-linux-gnueabi-gcc 2>/dev/null)
    _APT_SOFT := $(shell command -v arm-linux-gnueabi-gcc 2>/dev/null)
    ifneq ($(_MESSENSE),)
      CC_KINDLE := arm-unknown-linux-gnueabi-gcc
      STRIP_K   := arm-unknown-linux-gnueabi-strip
    else ifneq ($(_APT_SOFT),)
      CC_KINDLE := arm-linux-gnueabi-gcc
      STRIP_K   := arm-linux-gnueabi-strip
    else
      CC_KINDLE := arm-unknown-linux-gnueabi-gcc   # will fail with useful error
      STRIP_K   := arm-unknown-linux-gnueabi-strip
    endif
  endif
else
  $(error Unknown TARGET '$(TARGET)' — must be 'sf' or 'hf')
endif

CC_NATIVE  := gcc
BUILD_DIR  := build-kindle-$(TARGET)
DIST       := dist-$(TARGET)/airplay.koplugin

# ── ffmpeg flags ──────────────────────────────────────────────────
# For kindle target: we only need headers at compile time.
# KOReader ships its own ffmpeg on device; we link -lavcodec etc. at
# runtime via RPATH, so the host ffmpeg libs are compile-time only.
ifdef FFMPEG_PREFIX
  FF_CFLAGS  := -I$(FFMPEG_PREFIX)/include
  FF_LDFLAGS := -L$(FFMPEG_PREFIX)/lib -lavcodec -lavutil -lswscale
else
  FF_CFLAGS  := $(shell pkg-config --cflags libavcodec libavutil libswscale 2>/dev/null)
  FF_LDFLAGS := $(shell pkg-config --libs   libavcodec libavutil libswscale 2>/dev/null)
endif

# ── OpenSSL + libplist (ARM static, set by Dockerfile CMD) ───────
# These are empty for native builds (pkg-config path used instead).
OPENSSL_CFLAGS  ?=
OPENSSL_LDFLAGS ?= $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")
PLIST_CFLAGS    ?=
PLIST_LDFLAGS   ?= $(shell pkg-config --libs libplist-2.0 2>/dev/null || echo "-lplist-2.0")

# ── Common compile/link flags ─────────────────────────────────────
UXPLAY_INC  := -I$(UXPLAY) -I$(UXPLAY)/llhttp
CFLAGS      := -O2 -Wall -Wextra -fPIC -Isrc $(UXPLAY_INC) $(FF_CFLAGS) \
               $(OPENSSL_CFLAGS) $(PLIST_CFLAGS) \
               -DPLIST_210 \
               -Wno-unused-parameter -Wno-sign-compare
LDFLAGS     := -shared -lpthread -Wl,--wrap,llhttp_init $(FF_LDFLAGS) $(OPENSSL_LDFLAGS) $(PLIST_LDFLAGS)
# ARM: static libs linked directly — no runtime dependency.
# FF_LDFLAGS, OPENSSL_LDFLAGS, PLIST_LDFLAGS set by Dockerfile CMD.
LDFLAGS_ARM := -shared -lpthread -ldl -Wl,--wrap,llhttp_init $(FF_LDFLAGS) $(OPENSSL_LDFLAGS) $(PLIST_LDFLAGS)

# ─────────────────────────────────────────────────────────────────
.PHONY: kindle native docker-kindle clean help

# ── kindle: cross-compile on Mac via toolchain ────────────────────
kindle: _check_cross $(BUILD_DIR)/$(LIB)
	@$(MAKE) --no-print-directory _assemble SO=$(BUILD_DIR)/$(LIB)

_check_cross:
	@command -v $(CC_KINDLE) >/dev/null 2>&1 || { \
	  echo ""; \
	  echo "ERROR: ARM cross-compiler not found for TARGET=$(TARGET) ($(CC_KINDLE))"; \
	  echo ""; \
	  if [ "$(TARGET)" = "hf" ]; then \
	    echo "Install the HF (hard-float) toolchain:"; \
	    echo "  download 'kindlehf.tar.gz' from"; \
	    echo "  https://github.com/koreader/koxtoolchain/releases"; \
	    echo "  extract to ~/x-tools and add .../arm-kindlehf-linux-gnueabihf/bin to PATH"; \
	  else \
	    echo "Install on Mac:"; \
	    echo "  brew tap messense/macos-cross-toolchains"; \
	    echo "  brew install arm-unknown-linux-gnueabi"; \
	  fi; \
	  echo ""; \
	  echo "Or use Docker (no install needed):"; \
	  echo "  make docker-kindle TARGET=$(TARGET)"; \
	  echo ""; \
	  exit 1; }

$(BUILD_DIR)/$(LIB): $(SRC) $(HDR)
	@mkdir -p $(BUILD_DIR)
	$(CC_KINDLE) $(CFLAGS) $(ARM_CFLAGS) -o $@ $(SRC) $(LDFLAGS_ARM)
	$(STRIP_K) --strip-unneeded $@

# ── docker-kindle: build via Dockerfile.$(TARGET) ─────────────────
DOCKER_IMAGE := airplay-kindle-builder-$(TARGET)

docker-kindle:
	@command -v docker >/dev/null 2>&1 || \
	  { echo "Install Docker: brew install --cask docker"; exit 1; }
	docker build -t $(DOCKER_IMAGE) -f Dockerfile.$(TARGET) .
	docker run --rm \
	  -v "$(CURDIR)":/work \
	  -e TARGET=$(TARGET) \
	  $(DOCKER_IMAGE)
	@$(MAKE) --no-print-directory _assemble SO=$(BUILD_DIR)/$(LIB)

# ── native: build for Mac (test only, not for Kindle) ─────────────
# Always sf-shaped (no compat_glibc.c — that's a GNU/Linux-only symver
# shim and won't compile with macOS's linker anyway).
native: build-native/$(LIB)
	@$(MAKE) --no-print-directory _assemble SO=build-native/$(LIB) DIST=dist/airplay.koplugin

build-native/$(LIB): $(SRC_COMMON) $(HDR)
	@mkdir -p build-native
	$(CC_NATIVE) $(CFLAGS) -o $@ $(SRC_COMMON) $(LDFLAGS)

# ── assemble dist/ ────────────────────────────────────────────────
.PHONY: _assemble
_assemble:
	@mkdir -p $(DIST)
	cp $(SO) $(DIST)/$(LIB)
	cp plugin/_meta.lua       $(DIST)/
	cp plugin/main.lua        $(DIST)/
	cp plugin/airplay_ffi.lua $(DIST)/
	@echo ""
	@echo "Plugin ready:  $(DIST)/"
	@echo ""
	@echo "Deploy to Kindle (USB):"
	@echo "  cp -r $(DIST) /Volumes/Kindle/extensions/koreader/plugins/"

clean:
	rm -rf build-kindle-sf build-kindle-hf build-native dist dist-sf dist-hf

help:
	@echo "Targets:"
	@echo "  make kindle TARGET=sf|hf         cross-compile for Kindle ARM"
	@echo "  make docker-kindle TARGET=sf|hf  same but via Docker (needs Docker Desktop)"
	@echo "  make native                      build for Mac host (testing only)"
	@echo "  make clean"
	@echo ""
	@echo "TARGET=sf (default): firmware <=5.16.2.1.1, soft-float ABI"
	@echo "TARGET=hf:           firmware >=5.16.3,      hard-float ABI (PW6/PW12, Scribe)"
	@echo ""
	@echo "Mac setup — Option A (native toolchain):"
	@echo "  sf: brew tap messense/macos-cross-toolchains && brew install arm-unknown-linux-gnueabi"
	@echo "  hf: extract koxtoolchain's kindlehf.tar.gz release to ~/x-tools, add bin/ to PATH"
	@echo ""
	@echo "Mac setup — Option B (Docker):"
	@echo "  brew install --cask docker"
	@echo "  make docker-kindle TARGET=hf"
