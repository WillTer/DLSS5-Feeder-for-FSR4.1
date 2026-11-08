# Minimal minidump reader: exception record + faulting module + raw stack scan.
# No dependencies. Usage: python -I minidump.py dump.dmp
import struct, sys

def parse(path):
    b = open(path, "rb").read()
    sig, ver, nstreams, dirrva = struct.unpack_from("<IIII", b, 0)
    assert sig == 0x504D444D, "not MDMP"
    streams = {}
    for i in range(nstreams):
        st, ds, rva = struct.unpack_from("<III", b, dirrva + 12 * i)
        streams.setdefault(st, (ds, rva))

    # modules
    mods = []
    if 4 in streams:
        _, rva = streams[4]
        (n,) = struct.unpack_from("<I", b, rva)
        off = rva + 4
        for i in range(n):
            base, size = struct.unpack_from("<QI", b, off)
            name_rva = struct.unpack_from("<I", b, off + 20)[0]
            (ln,) = struct.unpack_from("<H", b, name_rva)
            name = b[name_rva + 4: name_rva + 4 + ln].decode("utf-16le", "replace")
            mods.append((base, size, name))
            off += 108

    def where(addr):
        for base, size, name in mods:
            if base <= addr < base + size:
                return f"{name}+0x{addr - base:X}"
        return None

    print(f"== {path}")
    print(f"modules: {len(mods)}")
    # exception
    if 6 in streams:
        _, rva = streams[6]
        tid, _ = struct.unpack_from("<II", b, rva)
        code, flags, rec, addr, nparams = struct.unpack_from("<IIQII", b, rva + 8)
        print(f"exception: code=0x{code:08X} addr=0x{addr:016X} thread={tid}")
        print(f"  at: {where(addr) or 'unmapped'}")
        ctx_size, ctx_rva = struct.unpack_from("<II", b, rva + 8 + 152)
        # WOW64 dumps: the primary context is x64 (Rip 0xF0, Rsp 0x98, Rbp 0xA0);
        # native 32-bit dumps use Eip 0xB8 / Esp 0xC4. Detect by address width.
        is64 = addr > 0xFFFFFFFF
        if is64:
            rip, rsp, rbp = (struct.unpack_from("<Q", b, ctx_rva + 0xF0)[0],
                             struct.unpack_from("<Q", b, ctx_rva + 0x98)[0],
                             struct.unpack_from("<Q", b, ctx_rva + 0xA0)[0])
        else:
            rip, rsp, rbp = (struct.unpack_from("<I", b, ctx_rva + 0xB8)[0],
                             struct.unpack_from("<I", b, ctx_rva + 0xC4)[0],
                             struct.unpack_from("<I", b, ctx_rva + 0xB4)[0])
        print(f"context: {'rip' if is64 else 'eip'}=0x{rip:X} ({where(rip)})  rsp=0x{rsp:X}  rbp=0x{rbp:X}")
        # find stack memory
        stack = None
        if 9 in streams:  # Memory64List
            _, rva9 = streams[9]
            (nranges,) = struct.unpack_from("<Q", b, rva9)
            (base_rva,) = struct.unpack_from("<Q", b, rva9 + 8)
            off = rva9 + 16
            for i in range(nranges):
                start, dsize = struct.unpack_from("<QQ", b, off)
                if start <= rsp < start + dsize:
                    stack = (start, base_rva, dsize)
                    break
                base_rva += dsize
                off += 16
        if stack is None and 5 in streams:  # MemoryListStream
            _, rva5 = streams[5]
            (nranges,) = struct.unpack_from("<I", b, rva5)
            for i in range(nranges):
                start = struct.unpack_from("<Q", b, rva5 + 4 + 16 * i)[0]
                dsize, rva_m = struct.unpack_from("<II", b, rva5 + 4 + 16 * i + 8)
                if start <= rsp < start + dsize:
                    stack = (start, rva_m, dsize)
                    break
        # key modules present
        for base, size, name in mods:
            low = name.lower()
            if any(k in low for k in ("d3d9", "d3d8", "ddraw", "dxgi", "d3d11", "reshade", "spellforce.exe", "dinput")):
                print(f"  module: 0x{base:016X} {name}")
        if stack:
            start, data_rva, dsize = stack
            lo = rsp - start
            data = b[data_rva + lo: data_rva + min(dsize, lo + 8192)]
            print("stack scan (return-address candidates):")
            shown, step = 0, 8 if is64 else 4
            fmt = "<Q" if is64 else "<I"
            for i in range(0, len(data) - step, step):
                (v,) = struct.unpack_from(fmt, data, i)
                w = where(v)
                if w and shown < 30:
                    print(f"  rsp+0x{i:03X}: 0x{v:X} {w}")
                    shown += 1
    else:
        print("no exception stream")

if __name__ == "__main__":
    for p in sys.argv[1:]:
        try:
            parse(p)
        except Exception as e:
            print(f"== {p}: parse failed: {e}")
