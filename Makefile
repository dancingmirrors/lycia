CC?=cc
PREFIX?=/usr/local
CFLAGS+=-Wall -Wextra -Wpedantic

lycia: lycia.c

install:
	install -Dm 0755 lycia ${PREFIX}/bin

uninstall:
	rm ${PREFIX}/bin/lycia

clean:
	rm lycia
