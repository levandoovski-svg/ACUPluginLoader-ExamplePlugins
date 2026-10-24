#!/usr/bin/env python3
"""Read-only runtime image dumper for Denuvo-packed ACU.exe.

REQUIRES: ACU.exe running (main menu reached -> Denuvo has unpacked .text).
Performs NO injection and NO writes to the target: OpenProcess with
PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION only,
then VirtualQueryEx + ReadProcessMemory.

Output: a load-aligned image file (file offset == RVA). If the original
FileAlignment == SectionAlignment, this file is directly importable as a PE
by Ghidra at the recorded actual base. A sidecar JSON records the real
base, ASLR delta vs 0x140000000, and per-region read coverage.

Usage: python dump_acu.py [--pid N] [--out DIR]
"""
import argparse, ctypes, ctypes.wintypes as wt, json, os, struct, sys, time

k32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)

PROCESS_VM_READ = 0x0010
PROCESS_VM_OPERATION = 0x0008
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_char * 260)]

class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
                ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE), ("szModule", ctypes.c_char * 256),
                ("szExePath", ctypes.c_char * 260)]

class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]

def find_pid(name=b"ACU.exe"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = ctypes.sizeof(pe)
    ok = k32.Process32First(snap, ctypes.byref(pe))
    hits = []
    while ok:
        if pe.szExeFile == name:
            hits.append(pe.th32ProcessID)
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return hits

def module_base(pid, name=b"ACU.exe"):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
    ok = k32.Module32First(snap, ctypes.byref(me))
    while ok:
        if me.szModule == name:
            base = ctypes.cast(me.modBaseAddr, ctypes.c_void_p).value
            k32.CloseHandle(snap)
            return base, me.modBaseSize
        ok = k32.Module32Next(snap, ctypes.byref(me))
    k32.CloseHandle(snap)
    return None, None

def read_remote(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    ok = k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got))
    if not ok:
        return None, 0
    return buf.raw[:got.value], got.value

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--wait", type=int, default=0, help="seconds to wait for ACU.exe to appear")
    a = ap.parse_args()

    pid = a.pid
    deadline = time.time() + a.wait
    while pid == 0:
        pids = find_pid()
        if pids:
            pid = pids[0]; break
        if time.time() > deadline:
            print("ACU.exe not found; start the game (reach main menu) and retry."); sys.exit(2)
        time.sleep(2)
    print(f"pid={pid}")

    rights = PROCESS_VM_READ | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION
    h = k32.OpenProcess(rights, False, pid)
    if not h:
        rights2 = PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION
        h = k32.OpenProcess(rights2, False, pid)
    if not h:
        print(f"OpenProcess failed err={ctypes.get_last_error()}"); sys.exit(3)

    base, modsize = module_base(pid)
    if base is None:
        print("module ACU.exe not found in process"); sys.exit(4)
    default_base = 0x140000000
    delta = base - default_base
    print(f"module base={base:#x} size={modsize:#x} aslr_delta={delta:#x}")

    hdr, n = read_remote(h, base, 0x1000)
    if not hdr:
        print("cannot read headers"); sys.exit(5)
    e = struct.unpack_from("<I", hdr, 0x3C)[0]
    nsec = struct.unpack_from("<H", hdr, e + 6)[0]
    optsz = struct.unpack_from("<H", hdr, e + 20)[0]
    opt = e + 24
    entry = struct.unpack_from("<I", hdr, opt + 16)[0]
    salign = struct.unpack_from("<I", hdr, opt + 32)[0]
    falign = struct.unpack_from("<I", hdr, opt + 36)[0]
    sizeimg = struct.unpack_from("<I", hdr, opt + 56)[0]
    secoff = opt + optsz
    print(f"nsec={nsec} entry={entry:#x} salign={salign:#x} falign={falign:#x} sizeimg={sizeimg:#x}")

    image = bytearray(sizeimg)
    image[0:0x1000] = hdr
    coverage = []
    for i in range(nsec):
        o = secoff + i * 40
        name = hdr[o:o+8].rstrip(b"\0").decode(errors="replace")
        vsz, va, rsz, _ = struct.unpack_from("<IIII", hdr, o + 8)
        span = max(vsz, rsz)
        if span == 0:
            continue
        start = base + va
        end = start + span
        got_total = 0
        addr = start
        while addr < end:
            mbi = MEMORY_BASIC_INFORMATION()
            r = k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi))
            if not r:
                addr += 0x1000; continue
            rbase = mbi.BaseAddress or addr
            rsize = mbi.RegionSize
            rend = rbase + rsize
            if mbi.State != MEM_COMMIT or (mbi.Protect & PAGE_NOACCESS) or (mbi.Protect & PAGE_GUARD):
                addr = rend; continue
            lo = max(addr, rbase); hi = min(end, rend)
            if hi > lo:
                data, got = read_remote(h, lo, hi - lo)
                if data:
                    image[lo - base: lo - base + len(data)] = data
                    got_total += len(data)
            addr = rend
        coverage.append({"section": name, "rva": va, "span": span, "bytes_read": got_total,
                         "percent": round(100.0 * got_total / span, 2)})
        print(f"section {name:8} rva={va:#010x} span={span:#010x} read={got_total:#010x} ({coverage[-1]['percent']}%)")

    out = os.path.join(a.out, "ACU_dumped.bin")
    with open(out, "wb") as f:
        f.write(image)
    sidecar = {
        "pid": pid, "actual_base": base, "default_base": default_base,
        "aslr_delta": delta, "size_of_image": sizeimg, "entry_rva": entry,
        "section_alignment": salign, "file_alignment": falign,
        "import_hint": ("raw-binary entry_point=0x%x" % base) if True else "",
        "sections": coverage,
        "note": "file offset == RVA; load-aligned runtime image; IAT may hold resolved runtime DLL addresses",
    }
    with open(out + ".json", "w") as f:
        json.dump(sidecar, f, indent=2)
    print(f"wrote {out} ({len(image)} bytes) + sidecar {out}.json")

if __name__ == "__main__":
    main()
