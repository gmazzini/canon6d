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

The source follows C89 declaration/syntax rules. `gnu89` is used only because ISO C89 itself does not accept `//` comments, which are the required project comment style.

## Usage

```sh
./canon6dget IP PORT PATH
```

Example:

```sh
./canon6dget 192.168.31.200 15740 ~/Pictures/Canon6D
```

`PATH` is created if the final directory does not already exist.

## Behaviour

- Connects directly to the camera IP and TCP port supplied on the command line.
- Uses PTP/IP.
- Uses the same client GUID, host name and protocol version used by Airmtp 1.1, so an EOS 6D Wi-Fi profile already paired with Airmtp should be reusable.
- Enumerates every storage and every object.
- Downloads every non-folder object, regardless of file extension or format.
- Existing destination files are replaced.
- A download is first written as `filename.part`; the final destination file is replaced only after the complete transfer succeeds.
- Nothing is deleted or modified on the camera.
- Full files are transferred with `GetPartialObject` in 1 MiB blocks.
- There is no download history, filtering, renaming engine, discovery or configuration file.

## Canon EOS 6D Wi-Fi setup

The camera can be connected to the same access point as the Mac.

A known working example:

```text
Camera IP: 192.168.31.200
Port:      15740
```

During the first pairing, leave the camera on its computer-pairing screen and start the client. Canon cameras associate a Wi-Fi connection profile with the client GUID.

If the existing profile was paired with the Airmtp setup used previously, this program deliberately presents Airmtp 1.1's default client identity for compatibility.

## Current status

Version 1.02 has been compile-checked, but it has not yet been tested against the physical EOS 6D.

The first real-camera test should be:

```sh
./canon6dget 192.168.31.200 15740 ./test-download
```

Check that:

1. pairing/session setup completes;
2. JPG and CR2 files are both listed and downloaded;
3. downloaded sizes match the files on the camera;
4. running the command a second time replaces the local files;
5. an interrupted transfer leaves only the `.part` file and does not replace a previously complete file.

## Design scope

This is intentionally not a general Airmtp replacement. It implements only the workflow needed here: connect to a Canon EOS 6D and download all files.

## Version 1.02

- Corrected Canon object enumeration parameter.
- Corrected storage diagnostic control flow.
- Added up to 30 TCP connection attempts, one second apart, to tolerate Canon Wi-Fi startup latency.
