# ftdi-nand-flash-tool
NAND flash reader/programmer using a FTDI FT2232 IC in bit-bang mode

## General

This flash tool can be used to read (dump) and program parallel x8 bare
NAND flash chips. The target used was a TSOP48 Toshiba 256MiB chip, and
the code is reasonably tied to the specs of this chip but should be very
easily adapted (or made more generic) for any other parallel NAND chip.

Note: big-banging over USB is **slow**....

For a "real" tool to work with raw flash, see the
[dumpflash](https://github.com/ohjeongwook/dumpflash) project. It uses
the same FTDI FT2232 chip in "Host bus emulation mode" which is much
faster but was giving me trouble.

This tool is a good start learning about the flash protocol without
any magic done by the FDTI chip. It is very raw and will most likely
require some good tweaking to work with any chip other than the
expected Toshiba chip.

## Usage

Dump the whole flash using:
```shell
./flash-tool -f output.bin
```

Reprogram an empty chip (after erasing first) with:
```shell
./flash-tool -p output.bin
```

Erase a whole chip (note: will obliterate factory bad blocks, BAD!):
```shell
./flash-tool -E
```


## Hardware and Wiring

The NAND flash reader / programmer can be put together easily using a
FT2232 breakout board (for example, a DLP-2232H that is breadboard
friendly) and a TSOP48-&gt;DIP adapter.

These are the main signals to be connected:

| FT2232 | NAND | Signal Name          |
|--------|------|----------------------|
| ADBUS0 | IO0  | Data bus bit 0       |
| ADBUS1 | IO1  | Data bus bit 1       |
| ADBUS2 | IO2  | Data bus bit 2       |
| ADBUS3 | IO3  | Data bus bit 3       |
| ADBUS4 | IO4  | Data bus bit 4       |
| ADBUS5 | IO5  | Data bus bit 5       |
| ADBUS6 | IO6  | Data bus bit 6       |
| ADBUS7 | IO7  | Data bus bit 7       |
| BDBUS0 | CLE  | Command Latch Enable |
| BDBUS1 | ALE  | Address Latch Enable |
| BDBUS2 | CE#  | Chip Enable (Low)    |
| BDBUS3 | WE#  | Write Enable (Low)   |
| BDBUS4 | RE#  | Read Enable (Low)    |
| BDBUS5 | WP#  | Write Protect (Low)  |
| BDBUS6 | RY/BY#| READY / BUSY (Low)  |

Typically the Ready/Busy signal (BDBUS6) will have to be pulled up with
a 10K resistor to 3V3.

The GND and 3V3 power pins (typically a pair on each side of the TSOP48
chip) will also have to be connected.


## Credits
Forked from [ftdi-nand-flash-reader](https://github.com/maehw/ftdi-nand-flash-reader)
y [maehw](https://github.com/maehw).
