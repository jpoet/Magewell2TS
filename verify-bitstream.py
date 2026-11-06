#!/usr/bin/env python3
"""
verify-bitstream.py

Analyzes a raw MWCAP_AUDIO_CAPTURE_FRAME::adwSamples dump (raw-audio-allbits.bin)
to verify both symptoms of the Magewell 192 kHz Layout 0 HDMI audio demuxer bug:
  1. Symptom A: Subpacket 1 leaked one sample early into L1/R1.
  2. Symptom B: ~1.04% sample loss in L0/R0 (shrinking the 6,144-sample IEC 61937
     repetition interval to ~6,080 samples and truncating every E-AC-3 payload).
"""

import sys
import os
import struct


def main():
    filename = sys.argv[1] if len(sys.argv) > 1 else "raw-audio-allbits.bin"

    if not os.path.exists(filename):
        print(f"Error: File '{filename}' not found.", file=sys.stderr)
        print(f"Usage: {sys.argv[0]} [raw-audio-allbits.bin]", file=sys.stderr)
        sys.exit(1)

    with open(filename, "rb") as f:
        data = f.read()

    # Each sample frame is 8 channels x 4 bytes (32-bit, high-16-bit effective) = 32 bytes
    # Channel order: L0, L1, L2, L3, R0, R1, R2, R3
    stride = 32
    n_samples = len(data) // stride

    print(f"File: {filename} ({len(data):,} bytes, {n_samples:,} 8-channel sample rows)\n")

    # Unpack the upper 16 bits (bytes +2..+4 of each 32-bit word) for L0, L1, R0, R1
    l0 = [struct.unpack_from("<H", data, i * stride + 2)[0] for i in range(n_samples)]
    l1 = [struct.unpack_from("<H", data, i * stride + 6)[0] for i in range(n_samples)]
    r0 = [struct.unpack_from("<H", data, i * stride + 18)[0] for i in range(n_samples)]
    r1 = [struct.unpack_from("<H", data, i * stride + 22)[0] for i in range(n_samples)]

    # Locate all IEC 61937 burst preambles: PA = 0xF872 in L0, PB = 0x4E1F in R0
    syncs = [i for i in range(n_samples - 2) if l0[i] == 0xF872 and r0[i] == 0x4E1F]

    print(f"Found {len(syncs)} IEC 61937 bursts (PA=0xF872, PB=0x4E1F in L0/R0).\n")

    if len(syncs) < 2:
        print("Not enough bursts found to measure intervals.")
        sys.exit(0)

    # Check Symptom A across the entire file
    total_l1_leaks = 0
    echoed_in_next_l0 = 0
    for i in range(n_samples - 1):
        if l1[i] != 0 or r1[i] != 0:
            total_l1_leaks += 1
            if l0[i + 1] == l1[i] and r0[i + 1] == r1[i]:
                echoed_in_next_l0 += 1

    print("=== SYMPTOM A: Subpacket 1 Leaked into L1/R1 ===")
    print(f"Total rows with non-zero L1/R1 data : {total_l1_leaks:,} / {n_samples:,} samples")
    print(f"Rows where L1/R1 == next L0/R0 row  : {echoed_in_next_l0:,} / {total_l1_leaks:,}")
    print("-> Confirms L1/R1 is a 1-sample-early leak of L0/R0, NOT holding the dropped samples.\n")

    print("=== SYMPTOM B: Shrunken Burst Intervals & Truncated E-AC-3 Payloads ===")
    print(f"{'Burst':>5} | {'Sample':>7} | {'Byte Offset':>11} | {'Interval (Loss)':>16} | "
          f"{'PD Bytes (Samples)':>18} | {'Actual Samples':>14} | {'Payload Lost':>18}")
    print("-" * 105)

    total_interval_loss = 0
    total_payload_loss = 0
    num_bursts_to_show = len(syncs) - 1

    for b in range(num_bursts_to_show):
        s = syncs[b]
        next_s = syncs[b + 1]

        # Mandatory IEC 61937 E-AC-3 repetition period is 6,144 stereo samples
        interval = next_s - s
        interval_loss = 6144 - interval
        total_interval_loss += interval_loss

        # Row s+1 holds PC in L0 (0x0015 = E-AC-3) and PD in R0 (payload length in bytes for E-AC-3)
        pc = l0[s + 1]
        pd_bytes = r0[s + 1]
        expected_payload_samples = pd_bytes // 4  # 4 bytes per stereo sample (16-bit L0 + 16-bit R0)

        # Find the last non-zero payload sample in L0/R0 before the zero-padding gap
        last_nonzero = s + 2
        for k in range(s + 2, next_s):
            if l0[k] != 0 or r0[k] != 0:
                last_nonzero = k

        actual_payload_samples = last_nonzero - (s + 2) + 1
        payload_samples_lost = expected_payload_samples - actual_payload_samples
        payload_bytes_lost = payload_samples_lost * 4
        total_payload_loss += payload_samples_lost

        print(f"{b:5d} | {s:7d} |  0x{s * stride:08x} | "
              f"{interval:6d} (-{interval_loss:2d} smp) | "
              f"0x{pd_bytes:04x} ({expected_payload_samples:4d} smp) | "
              f"{actual_payload_samples:10d} smp | "
              f"-{payload_samples_lost:2d} smp (-{payload_bytes_lost:3d} B)")

    avg_int_loss = total_interval_loss / num_bursts_to_show
    avg_pay_loss = total_payload_loss / num_bursts_to_show
    print("-" * 105)
    print(f"Average lost per 6,144-sample period : {avg_int_loss:.2f} stereo samples ({avg_int_loss / 6144 * 100:.2f}%)")
    print(f"Average lost per E-AC-3 burst payload: {avg_pay_loss:.2f} stereo samples ({avg_pay_loss * 4:.1f} bytes)")


if __name__ == "__main__":
    main()
