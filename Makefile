UNAME := $(shell uname -s)
CC ?= cc
MUSL_GCC ?= musl-gcc
MINGW64 ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc

CFLAGS := -Os -ffunction-sections -fdata-sections
WINSOCK := -lws2_32

ifeq ($(UNAME),Darwin)
LDFLAGS := -Wl,-dead_strip
else
LDFLAGS := -Wl,--gc-sections
endif

all: tun

tun: tun.c
	$(CC) $(CFLAGS) $(LDFLAGS) -pthread tun.c -o tun

tun_macos_x86_64: tun.c
	$(CC) -target x86_64-apple-darwin $(CFLAGS) $(LDFLAGS) tun.c -o tun_macos_x86_64

tun_macos_arm64: tun.c
	$(CC) -target arm64-apple-darwin $(CFLAGS) $(LDFLAGS) tun.c -o tun_macos_arm64

tun_windows_x86_64.exe: tun.c
	$(MINGW64) $(CFLAGS) -Wl,--gc-sections -static -s $(WINSOCK) tun.c -o tun_windows_x86_64.exe

tun_windows_i686.exe: tun.c
	$(MINGW32) $(CFLAGS) -Wl,--gc-sections -static -s $(WINSOCK) tun.c -o tun_windows_i686.exe

clean:
	rm -f tun tun_linux_* tun_macos_* tun_windows_*.exe tun_*.sha256

.PHONY: all clean