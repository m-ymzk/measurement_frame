#!/usr/bin/env python3
import argparse
import socket
import statistics
import struct
from time import time_ns

SRC_MAC = 0x111122223333.to_bytes(6, byteorder="big")
ETHERTYPE_MFF = 0x3434
VERSION_MFF = 0x0002
DEVICE_ID = 0x00000001
NUM_INFO = 0x00000001
SHOT_ID_START = 0

CODE_DIST:int = 3
NUM_ROUND:int = 3

def parse_mac(mac: str) -> bytes:
    value = bytes.fromhex(mac.replace(":", ""))
    if len(value) != 6:
        raise ValueError(f"Invalid MAC address: {mac}")
    return value

def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Send a Measurement Frame Format (MFF) Ethernet II frame. "
            "The kick command fields are reused as step_id and device_id defaults."
        )
    )
    parser.add_argument("--interface", "-i", required=True, help="Interface NIC to send the frame")
    parser.add_argument("--dst-mac", "-d", required=True, help="Destination MAC address")
    parser.add_argument("--code-distance", "-c", type=int, default=CODE_DIST, help="Code distance (default: 3)")
    parser.add_argument("--num-shot", "-s", type=int, default=1, help="Number of shots to send")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print per-shot timing info")
    args = parser.parse_args()

    destination_mac = parse_mac(args.dst_mac)

    num_qubits = 2 * (args.code_distance ** 2) - 1
    len_payload = (num_qubits + 7) // 8

    with open("rand_record.bin", "rb") as rf:
        payload_data = memoryview(rf.read())

    if not payload_data:
        raise ValueError("rand_record.bin is empty")

    header = struct.pack("!6s6sHH", destination_mac, SRC_MAC, ETHERTYPE_MFF, VERSION_MFF)
    tail = struct.pack("!III", DEVICE_ID, num_qubits, NUM_ROUND)

    histry_latency = []

    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETHERTYPE_MFF)) as sock:
        sock.bind((args.interface, 0))
        for shot in range(args.num_shot):
            step_id = (time_ns() // 1) & 0xFFFFFFFFFFFFFFFF
            offset = (shot * len_payload) % len(payload_data)

            frame = bytearray(header)
            frame += struct.pack("!Q", step_id)
            frame += tail

            first_part = payload_data[offset:offset + len_payload]
            if len(first_part) < len_payload:
                frame.extend(first_part)
                remaining = len_payload - len(first_part)
                frame.extend(payload_data[:remaining])
            else:
                frame.extend(first_part)

            # Ethernet II minimum frame size is 60 bytes excluding FCS.
            if len(frame) < 60:
                frame.extend(b"\x00" * (60 - len(frame)))

            step_id_after = (time_ns() // 1) & 0xFFFFFFFFFFFFFFFF
            latency = step_id_after - step_id
            #print(f"# sended {step_id_after} {latency}us")
            #if latency < 1000: wait 1000-latency ns
            if latency < 1000:
                wait_ns = 1000 - latency
                wait_s = wait_ns / 1_000_000_000
                print(f"# wait {wait_ns}ns {wait_s}s")
                import time
                time.sleep(wait_s)
                step_id_after = (time_ns() // 1) & 0xFFFFFFFFFFFFFFFF
                latency = step_id_after - step_id
            histry_latency.append(latency)

            #sock.sendto(frame, (args.interface, 0))
            sock.send(frame)

            if args.verbose:
                # 8 バイトずつに改行を入れてframeをダンプ
                for i in range(0, len(frame), 8):
                    print(" ".join(f"{b:02x}" for b in frame[i:i+8]))

    history = histry_latency[2:]  # Skip the first two shots for more stable statistics
    if history:
        print(f"Average latency: {statistics.fmean(history):.1f} +- {statistics.pstdev(history):.1f}ns")
        print(f"Max latency: {max(history)}ns")
        print(f"Min latency: {min(history)}ns")

if __name__ == "__main__":
    main()
