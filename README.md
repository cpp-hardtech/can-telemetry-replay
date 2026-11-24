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
in either byte order and a single multiplexor per message. Values are printed before
DBC scaling and offsets. File order is preserved without waiting between frames.
The log format represents Classic CAN data frames. The DBC reader rejects
floating-point signals and extended multiplexing in replay mode.
DBC files may start with a UTF-8 byte order mark (BOM).

## Library components

- [DBC parser](dbc/README.md) dispatches callbacks for DBC messages, signals and attributes.
- [Telemetry transcoder](v2c/README.md) groups and aggregates CAN frames into packets.
  Enable `BUILD_EXAMPLES` to compile the separate live transcoder demonstration.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

Distributed under the three-clause BSD terms in [LICENSE](LICENSE). Additional
notices and source attribution are documented in [NOTICE](NOTICE).
