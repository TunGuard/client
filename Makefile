CC_amd64 := musl-gcc
CC_mips := mips-linux-musl-gcc
CC_arm64 := aarch64-linux-musl-gcc
CFLAGS := -Os -ffunction-sections -fdata-sections -Wl,--gc-sections
LDFLAGS := -static -s

all: tun

tun: tun.c
	$(CC_amd64) $(CFLAGS) $(LDFLAGS) tun.c -o tun

tun_mips: tun.c
	$(CC_mips) $(CFLAGS) $(LDFLAGS) tun.c -o tun_mips

tun_arm64: tun.c
	$(CC_arm64) $(CFLAGS) $(LDFLAGS) tun.c -o tun_arm64

clean:
	rm -f tun tun_x86_64 tun_mips tun_arm64

.PHONY: all clean