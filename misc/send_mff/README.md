# send_mff

This directory contains a direct Ethernet II sender for the Measurement Frame Format (MFF).

## Purpose

The program sends raw Layer-2 frames that match the frame format described in the project specification. It is intended for direct transmission of measurement-frame payloads without going through the higher-level control communication protocol.

## Build

From this directory:

```bash
cmake -S . -B build
cmake --build build --target send_measurement_frame_c -j2
```

## Run

```bash
sudo ./build/send_measurement_frame_c \
  -i eth0 \
  -d 11:22:33:44:55:66 \
  -s 100 \
  -c 3 \
  -v
```

## Options

- `-i`, `--interface`: interface name to send through
- `-d`, `--dst-mac`: destination MAC address
- `-s`, `--num-shot`: number of frames to send
- `-c`, `--code-distance`: code distance used to determine qubit count
- `-n`, `--info-num`: number of information bits
- `-v`: print latency summary

## Notes

- Raw socket access requires root privileges.
- The payload is read from `rand_record.bin` in the same directory.
- The frame uses Ethernet Type `0x3434` and Version `0x0002` as defined by the MFF specification.

- For example, you can create the rand_record.bin file for testing by using the following methods:
  $ dd if=/dev/urandom of=rand_record.bin bs=16k count=1