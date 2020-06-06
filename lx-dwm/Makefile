# lx-dwm - dynamic window manager
# See LICENSE file for copyright and license details.

include config.mk

SRC = drw.c lx-dwm.c util.c
OBJ = ${SRC:.c=.o}

all: options lx-dwm

options:
	@echo lx-dwm build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

config.h:
	cp config.def.h $@

lx-dwm: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f lx-dwm ${OBJ} lx-dwm-${VERSION}.tar.gz

dist: clean
	mkdir -p lx-dwm-${VERSION}
	cp -R LICENSE Makefile README config.def.h config.mk\
		lx-dwm.1 drw.h util.h ${SRC} lx-dwm.png transient.c lx-dwm-${VERSION}
	tar -cf lx-dwm-${VERSION}.tar lx-dwm-${VERSION}
	gzip lx-dwm-${VERSION}.tar
	rm -rf lx-dwm-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f lx-dwm ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/lx-dwm
#	mkdir -p ${DESTDIR}${MANPREFIX}/man1
#	sed "s/VERSION/${VERSION}/g" < lx-dwm.1 > ${DESTDIR}${MANPREFIX}/man1/lx-dwm.1
#	chmod 644 ${DESTDIR}${MANPREFIX}/man1/lx-dwm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/lx-dwm\
		${DESTDIR}${MANPREFIX}/man1/lx-dwm.1

.PHONY: all options clean dist install uninstall
