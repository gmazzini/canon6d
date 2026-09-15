# canon6dget

Minimal standalone C utility to download every file from a Canon EOS 6D over Wi-Fi/PTP-IP.

## Build

macOS:

```sh
cc -std=gnu89 -O2 -Wall -Wextra canon6dget.c -o canon6dget
```

Linux:

```sh
cc -std=gnu89 -O2 -Wall -Wextra canon6dget.c -o canon6dget
```

No external libraries are required.

The source follows C89 declaration and syntax rules. `gnu89` is used only because ISO C89 itself does not accept `//` comments, which are the required project comment style.

## Usage

```sh
./canon6dget IP PORT PATH
```

Example:

```sh
./canon6dget 10.0.0.20 15740 ~/Pictures/Canon6D
```

`PATH` is created if the final directory does not already exist.

## Behaviour

- Connects directly to the camera IP and TCP port supplied on the command line.
- Uses PTP/IP.
- Uses the client identity `airmtp` for compatibility with the Canon Wi-Fi pairing already associated with that client.
- Enumerates every storage and every object available on the camera.
- Downloads every non-folder object, regardless of file extension or format.
- Existing destination files are replaced automatically.
- Each download is first written as `filename.part`; the final destination file is replaced only after the complete transfer succeeds.
- Nothing is deleted or modified on the camera.
- Full files are transferred with `GetPartialObject` in 1 MiB blocks.
- The program retries the TCP connection for up to 30 seconds to tolerate Canon Wi-Fi startup latency.
- There is no download history, filtering, renaming engine, discovery or configuration file.

## Canon EOS 6D Wi-Fi setup

The camera can be connected to the same Wi-Fi access point as the computer.

A typical configuration is:

```text
Camera IP: 10.0.0.20
Port:      15740
Client:    airmtp
```

Start the Wi-Fi connection on the Canon EOS 6D first and leave the camera waiting for the computer. Then run `canon6dget`.

Example:

```sh
./canon6dget 10.0.0.20 15740 ~/Downloads
```

During the first pairing with a new camera connection profile, leave the camera on its computer-pairing screen and start `canon6dget`. The camera associates the Wi-Fi connection profile with the client identity used by the program.

## Design scope

`canon6dget` is intentionally not a general-purpose camera transfer utility. It implements one operation only: connect to a Canon EOS 6D and download all available files to the selected directory.
