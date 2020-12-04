CC = gcc
CFLAGS = -Wall
OBJECTS = backlight-dbus.o
EXEC = backlight-dbus
PREFIX = ~/.local

.PHONY: all clean install uninstall

all: backlight-dbus

backlight-dbus: ${OBJECTS}
		${CC} -o ${EXEC} ${OBJECTS} -lm -lsystemd

clean:
		rm -f *.o ${EXEC}

install:
		install -D -t ${PREFIX}/bin/ backlight-dbus
		install -D -t ${PREFIX}/share/man/man1/ backlight-dbus.1
		mandb -q

uninstall:
		rm -f ${PREFIX}/bin/backlight-dbus
		rm -f ${PREFIX}/share/man/man1/backlight-dbus.1
		mandb -q
