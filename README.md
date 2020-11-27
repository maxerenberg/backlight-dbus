# backlight-dbus
backlight-dbus - a backlight controller using DBus

## Synopsis
backlight-dbus [-h] [-v] [-d device_name] [-x session_id] [-t countdown] [brightness]

## Description
**backlight-dbus** is a small utility to adjust the backlight brightness of a
monitor using the DBus interface of systemd-logind. When called with no arguments,
the current and maximum brightness levels are printed.

## Building
Requirements:
* systemd header files (available in the package `libsystemd-dev` on Debian)
* gcc
* make

Once the requirements have been satisfied, you can simply run the following:
```
make
make install
```

## Options
* -h Show help message.
* -v Enable verbose output (debug messages).
* -d *device_name*

  The device name to control. This is a folder (usually a symlink) in
*/sys/class/backlight/*. If not specified, the first folder found
will be used.
* -x *session_id*

  The systemd-logind session ID of the current user. If not specified,
the environment variable XDG_SESSION_ID is first checked, and then
a list of all sessions is checked.
* -t *countdown*

  The number of seconds over which the brightness should fade. This can
be a floating point number.
* *brightness*

  This can be one of:
  * An absolute number, like the value in */sys/class/backlight/&lt;device_name&gt;/brightness*.
  * A plus (+) or minus (-) sign followed by an absolute number. The number will be
added/subtracted from the current brightness value.
  * A number followed by a percent sign (%). The brightness will be set to this
percentage of the maximum value.


  The +/- signs and the % sign may be combined together.

## Examples
`backlight-dbus -d acpi_video0 15`

`backlight-dbus -200`

`backlight-dbus -t 3.5 75%`

`backlight-dbus -10%`

## Notes
Currently, the program will run for longer than the specified countdown
because the time it takes to wait for a DBus method call to return is
not accounted for.

## See Also
* [xbacklight(1)](https://github.com/tcatm/xbacklight)

## Author
Max Erenberg