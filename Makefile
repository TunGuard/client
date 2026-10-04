UNAME := $(shell uname -s)
CC ?= cc
MUSL_GCC ?= musl-gcc
MINGW64 ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc

# Android is built with the NDK so the binary links against bionic, never glibc.
# Point ANDROID_NDK_HOME at an NDK install, or override the three compilers below.
NDK ?= $(ANDROID_NDK_HOME)
NDK_BIN ?= $(NDK)/toolchains/llvm/prebuilt/linux-$(shell uname -m)/bin
ANDROID_API ?= 21
ANDROID_CC_arm64 ?= $(NDK_BIN)/aarch64-linux-android$(ANDROID_API)-clang
ANDROID_CC_armv7 ?= $(NDK_BIN)/armv7a-linux-androideabi$(ANDROID_API)-clang
ANDROID_CC_x86_64 ?= $(NDK_BIN)/x86_64-linux-android$(ANDROID_API)-clang

CFLAGS := -Os -ffunction-sections -fdata-sections
WINSOCK := -lws2_32
# advapi32 backs the CryptoAPI device-id entropy source (CryptGenRandom).
WINSOCK_ALL := -lws2_32 -ladvapi32
ANDROID_CFLAGS := $(CFLAGS) -fPIE
ANDROID_LDFLAGS := -pie -Wl,--gc-sections -Wl,-z,relro,-z,now

ifeq ($(UNAME),Darwin)
LDFLAGS := -Wl,-dead_strip
else
LDFLAGS := -Wl,--gc-sections
endif

all: tun

tun: tun.c
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread tun.c -o tun

# ---- Linux (statically linked musl, no glibc) -------------------------------

tun_linux_x86_64: tun.c
	$(MUSL_GCC) $(CFLAGS) $(LDFLAGS) -static -s -pthread tun.c -o $@

tun_linux_arm64: tun.c
	aarch64-linux-musl-gcc $(CFLAGS) $(LDFLAGS) -static -s -pthread tun.c -o $@

# ---- macOS -------------------------------------------------------------------

tun_macos_x86_64: tun.c
	$(CC) -target x86_64-apple-darwin $(CFLAGS) $(LDFLAGS) tun.c -o $@

tun_macos_arm64: tun.c
	$(CC) -target arm64-apple-darwin $(CFLAGS) $(LDFLAGS) tun.c -o $@

# ---- Windows -----------------------------------------------------------------

# NOTE: -lws2_32 / -ladvapi32 must come *after* tun.c. GNU ld resolves left to
# right, so a library listed before the object that needs it is dropped as
# unused -- which is what produced the "undefined reference to `_imp__recv@16'"
# link failure.
tun_windows_x86_64.exe: tun.c
	$(MINGW64) $(CFLAGS) $(LDFLAGS) -static -s tun.c -o $@ $(WINSOCK_ALL)

tun_windows_i686.exe: tun.c
	$(MINGW32) $(CFLAGS) $(LDFLAGS) -static -s tun.c -o $@ $(WINSOCK_ALL)

# ---- Android (bionic, dynamically linked against the device libc) ------------
#
# Deliberately NOT -static: bionic rejects static executables on ARM/ARM64 below
# API 29 ("executable's TLS segment is underaligned"), and the on-device linker
# (/system/bin/linker64) plus libc.so ship with every Android release anyway.

tun_android_arm64-v8a: tun.c
	$(ANDROID_CC_arm64) $(ANDROID_CFLAGS) $(ANDROID_LDFLAGS) -pthread tun.c -o $@

tun_android_armeabi-v7a: tun.c
	$(ANDROID_CC_armv7) $(ANDROID_CFLAGS) $(ANDROID_LDFLAGS) -pthread tun.c -o $@

tun_android_x86_64: tun.c
	$(ANDROID_CC_x86_64) $(ANDROID_CFLAGS) $(ANDROID_LDFLAGS) -pthread tun.c -o $@

# ---- tests -------------------------------------------------------------------

# Exercises the P2P link against a fake peer over loopback. It includes tun.c
# directly, so it is built here and never shipped in a release.
test: tests/link_test.c tun.c
	$(CC) $(CFLAGS) -Wall -pthread tests/link_test.c -o tests/link_test
	./tests/link_test

# ---- aggregates --------------------------------------------------------------

linux: tun_linux_x86_64 tun_linux_arm64

macos: tun_macos_x86_64 tun_macos_arm64

windows: tun_windows_x86_64.exe tun_windows_i686.exe

android: tun_android_arm64-v8a tun_android_armeabi-v7a tun_android_x86_64

release: linux windows macos android

clean:
	rm -f tun tun_linux_* tun_macos_* tun_windows_*.exe tun_android_* tests/link_test

.PHONY: all linux macos windows android release clean test
