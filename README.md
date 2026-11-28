# CAN Telemetry Replay

Decode timestamped Classic CAN logs against a DBC file. The command-line tool
reads frames in file order and prints the raw integer signal values, which makes
recorded and synthetic traffic reproducible without a live CAN interface.

## Requirements

- A C++20 compiler and CMake 3.20 or newer.
- Boost headers for Spirit X3 and Endian. On Debian and Ubuntu, install `libboost-dev`.

## Build

```sh
git clone https://github.com/cpp-hardtech/can-telemetry-replay.git
cd can-telemetry-replay
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Use `cmake --install build --prefix "$PWD/install"` to place the executable in
`install/bin/`.

## Quick start

```sh
./build/can-telemetry-replay example/example.dbc example/replay.log
```

```text
0 003 GPSAltitude.GPSAltitude=-2
500 003 GPSAltitude.GPSAltitude=5
500 007 PowerState.PowerState=2
```

Each input line has a nonnegative timestamp in decimal milliseconds, a CAN
identifier in hexadecimal, and the hexadecimal payload. Use three identifier
digits for an 11-bit frame and eight for a 29-bit frame. Encode each payload byte
with two digits, or use `-` for an empty frame. Blank lines and lines starting with
`#` are accepted. Timestamps may repeat and must be nondecreasing.

Frames with unknown identifiers are skipped. A known frame must have the payload
length in its DBC definition. The tool supports signed and unsigned integer signals
in either byte order. Values are printed before DBC scaling and offsets. File order
is preserved without waiting between frames. The log format represents Classic CAN
data frames with integer DBC signals. DBC files may start with a UTF-8 byte
order mark (BOM).

Multiplexed signals appear when their selectors are active and match their DBC
conditions. `SG_MUL_VAL_` lists inclusive ranges, including disjoint ranges for
one selector. Nested selectors and signals with several selector conditions are
printed only when all their conditions match. `SG_MUL_VAL_` supplies the selector
conditions for a signal when present. A signal with only an inline marker such as
`m1` uses the message's single root selector.

Pass `--physical` before the DBC path to apply each signal's DBC factor and
offset and show its unit, such as `BatteryCurrent.BattVoltage=12.3 V`.
Multiplexor selectors remain raw IDs so they identify the active branch.
Use the default mode for the exact raw integer values.

Use `--id HEX` to show selected CAN identifiers in text, CSV, or aggregate
output. The argument uses the same width as the frame log. For example,
`--id 123` selects the standard identifier and `--id 00000123` selects the
extended identifier. Repeat `--id` to show any of the specified identifiers.
The tool checks frame syntax, timestamps, and known payload lengths throughout
the input log.

With `--candump`, use `--interface IFACE` to select an exact recorded
interface name. Repeat the option to include several interfaces. In text and
CSV output, a record must match both the interface selection and any `--id`
selection. All input frames still pass through the normal validation.

The extended multiplexing sample shows how nested selectors gate decoded signals.

```sh
./build/can-telemetry-replay --physical tests/extended-multiplex.dbc tests/extended-multiplex.log
```

When `Mode` is `3` and `Detail` is `6`, the sample reports `Offset=-7 A` and
`Temperature=1 C`. With `Mode=2` and `Detail=4`, only `Offset` is active.

Pass `--candump` to replay a compact Classic CAN capture, such as a file
written by `candump -L`. The output starts with the capture timestamp in
seconds and the interface name, so records from different interfaces can be
distinguished. Both microsecond and nanosecond timestamps are supported.
Frames are replayed in capture file order even if their timestamps decrease,
which can happen in captures taken across multiple interfaces.
Combine `--candump` and `--physical` to display engineering values from a
capture.

Use `--csv` for downstream analysis. Each row includes the timestamp, its unit
(`ms` for the three-column input or `s` for `--candump`), interface, hexadecimal
identifier, message name, signal name, value, and engineering unit when applied.
Raw signed and unsigned values are written as integers. The interface column is
empty for the three-column input.

## Read a log from standard input

Pass `-` as the log path to read from standard input. This lets you decompress
a candump capture while exporting it to CSV.

```sh
gzip -dc capture.log.gz | ./build/can-telemetry-replay --candump --csv example/example.dbc -
```

## Aggregate a recorded log

```sh
./build/can-telemetry-replay --aggregate example/example.dbc example/energy-replay.log
```

The example DBC groups battery current with state of charge every 500 ms. Its
first window reports the latest state of charge (`20`) and the average raw values
for the battery signals.

```text
500 004 SOC.SOCavg=20
500 006 BatteryCurrent.RawBattCurrent=20
500 006 BatteryCurrent.SmoothBattCurrent=30
500 006 BatteryCurrent.BattVoltage=110
```

The first recorded timestamp starts the clock, and each group closes its window
at the period defined in the DBC. A frame exactly at the closing timestamp belongs
to the next window. Every configured group member must arrive within that window
for it to appear in the output. Each result carries its window closing timestamp.
At end of file, the command emits any completed windows still waiting for the
next `V2CTxTime` publication and stops at the final recorded timestamp.

Aggregate replay uses the three-column log and prints raw integer values.
It supports a single, non-nested multiplexor per message. A filter supplied
with `--id` selects results after the aggregation windows have been assembled.

For a multiplexed message, the latest frame carries the common signals.
When timestamps tie, the later input record supplies those values.

## Library components

- [DBC parser](dbc/README.md) dispatches callbacks for DBC messages, signals and attributes.
- [Telemetry transcoder](v2c/README.md) groups and aggregates CAN frames into packets.
  Enable `BUILD_EXAMPLES` to compile the separate live transcoder demonstration.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Distributed under the three-clause BSD terms in [LICENSE](LICENSE).
