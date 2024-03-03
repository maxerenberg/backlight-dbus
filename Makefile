CFLAGS += -std=gnu11 -O2 -pipe -Wall -Wextra -Wno-unused-parameter
LDFLAGS += -lsystemd
EXEC = backlight-dbus
PREFIX ?= ~/.local

.PHONY: clean install uninstall

$(EXEC): backlight-dbus.c
		$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
		$(RM) $(EXEC)

install: $(EXEC)
		install -D -t $(PREFIX)/bin/ $(EXEC)
		install -D -t $(PREFIX)/share/man/man1/ backlight-dbus.1
		mandb -q

uninstall:
		$(RM) $(PREFIX)/bin/$(EXEC)
		$(RM) $(PREFIX)/share/man/man1/backlight-dbus.1
		mandb -q
