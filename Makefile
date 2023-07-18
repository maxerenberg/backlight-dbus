CC = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter
OBJECTS = backlight-dbus.o
EXEC = backlight-dbus
PREFIX = ~/.local

.PHONY: all clean install uninstall

all: backlight-dbus

${EXEC}: ${OBJECTS}
		${CC} -o ${EXEC} ${OBJECTS} -lsystemd

clean:
		rm -f *.o ${EXEC}

install:
		install -D -t ${PREFIX}/bin/ ${EXEC}
		install -D -t ${PREFIX}/share/man/man1/ backlight-dbus.1
		mandb -q

uninstall:
		rm -f ${PREFIX}/bin/${EXEC}
		rm -f ${PREFIX}/share/man/man1/backlight-dbus.1
		mandb -q
