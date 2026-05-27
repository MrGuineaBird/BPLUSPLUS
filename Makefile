CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

all: bpp

bpp: bpp.c
	$(CC) $(CFLAGS) bpp.c -o bpp -lm

install: bpp
	mkdir -p "$(BINDIR)"
	cp bpp "$(BINDIR)/bpp"

uninstall:
	rm -f "$(BINDIR)/bpp"

windows-setup: setup.c
	$(CC) $(CFLAGS) setup.c -o "B++ Setup.exe" -mwindows -lshell32 -ladvapi32 -lole32

clean:
	rm -f bpp bpp.exe "B++ Setup.exe" native_demo native_demo.exe native_demo.c native_demo.txt
