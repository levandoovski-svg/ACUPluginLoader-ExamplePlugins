#!/usr/bin/env python3
"""Rebuild a Ghidra-importable PE from the load-aligned runtime dump.

Input : ACU_dumped.bin (file offset == RVA) + ACU_dumped.bin.json sidecar
Output: ACU_dumped_pe.exe — same headers with FileAlignment forced to
        SectionAlignment (0x1000), each section's PointerToRawData=VirtualAddress
        and SizeOfRawData=VirtualSize (rounded up to 0x1000). Checksum zeroed,
        certificate directory cleared. Base stays 0x140000000 (no ASLR), so all
        library-proven addresses map 1:1.

Limitation (kept explicit): IAT in the dump holds runtime-resolved addresses of
real DLL functions, not thunk RVAs; Ghidra external refs will reflect that.
"""
import json, os, struct, sys

def align(v, a): return (v + a - 1) & ~(a - 1)

def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "ACU_dumped.bin")
    img = bytearray(open(dump, "rb").read())
    side = json.load(open(dump + ".json"))
    if side.get("aslr_delta") != 0:
        print(f"WARNING: aslr_delta={side['aslr_delta']:#x} nonzero; rebuild assumes delta 0")

    e = struct.unpack_from("<I", img, 0x3C)[0]
    nsec = struct.unpack_from("<H", img, e + 6)[0]
    optsz = struct.unpack_from("<H", img, e + 20)[0]
    opt = e + 24
    salign = struct.unpack_from("<I", img, opt + 32)[0]
    falign_off = opt + 36
    sizeimg = struct.unpack_from("<I", img, opt + 56)[0]

    # data directories: clear certificate (index 4)
    ddcount = struct.unpack_from("<I", img, opt + 108)[0]
    if ddcount > 4:
        struct.pack_into("<II", img, opt + 112 + 4 * 8, 0, 0)

    struct.pack_into("<I", img, falign_off, 0x1000)

    secoff = opt + optsz
    for i in range(nsec):
        o = secoff + i * 40
        name = bytes(img[o:o+8]).rstrip(b"\0").decode(errors="replace")
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", img, o + 8)
        new_raw = align(vsz, 0x1000)
        struct.pack_into("<II", img, o + 16, new_raw, va)
        print(f"section {name:8} rva={va:#010x} vsize={vsz:#010x} rawptr {rptr:#010x} -> {va:#010x}, rawsize {rsz:#010x} -> {new_raw:#010x}")

    # zero checksum
    struct.pack_into("<I", img, opt + 64, 0)
    out = dump.replace(".bin", "_pe.exe")
    open(out, "wb").write(bytes(img[:sizeimg]))
    print(f"wrote {out} ({sizeimg} bytes); import at base 0x140000000")

if __name__ == "__main__":
    main()
