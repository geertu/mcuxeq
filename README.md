# Microcontroller Command/Response Utility

"mcuxeq" is a small tool to send a command to a microcontroller attached to a
serial port, and read back the response, if any.

## Background

Microcontrollers implementing a shell interface over a serial port can be
controlled easily during interactive use.

While command line tools like "echo", "cat", and "screen -X stuff" can be used
to send commands from scripts, reading back responses is much harder.
Hence "mcuxeq" was born!

Summarized, mcuxeq sends a command over a serial link, waits for its echo to
appear, and prints all received data after that, until the shell's prompt is
seen.

## Features

  - Locking for atomic send/receive handling,
  - Retry on busy, which can be overridden by the super user,
  - Configurable serial port, expected prompt, and timeout.

## Usage

    mcuxeq: [options] [--] <command> ...

    Valid options are:
        -h, --help              Display this usage information
        -s, --device <dev>      Serial device to use
                                (Default: value of $MCUXEQ_DEV if set)
        -p, --prompt <prompt>   Expected prompt regex
                                (Default: value of $MCUXEQ_PROMPT if set)
                                (Default: "^[[:alnum:]]*[#$>] $")
        -t, --timeout <ms>      Timeout value in milliseconds
                                (Default: 2000)
        -d, --debug             Increase debug level
        -f, --force             Force open when busy (needs CAP_SYS_ADMIN)

Note that you can send control codes (e.g. "CTRL-C") by prefixing them with
"CTRL-V".

## Examples

  * Pulse GPIO zero on the BCU/2 connected to /dev/ttyUSB0:

        $ mcuxeq -s /dev/ttyUSB0 gpio 0 pulse
        $

  * Sample all power channels on the BCU/2 connected to /dev/ttyUSB0:

        $ export MCUXEQ_DEV=/dev/ttyUSB0
        $ mcuxeq sample all
        0.000 V / 0.000 A / 0.000 W
        0.000 V / 0.000 A / 0.000 W
        $
