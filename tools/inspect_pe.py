# Dump a PE's import table (DLL names + interesting functions) with no dependencies.
import struct, sys

def rva_to_off(rva, sections):
    for va, vs, ra, rs in sections:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va)
    return None

def cstr(buf, off):
    end = buf.index(b"\0", off)
    return buf[off:end].decode("latin1")

def imports(path):
    buf = open(path, "rb").read()
    pe = struct.unpack_from("<I", buf, 0x3C)[0]
    assert buf[pe:pe+4] == b"PE\0\0", "not a PE"
    machine = struct.unpack_from("<H", buf, pe + 4)[0]
    nsec    = struct.unpack_from("<H", buf, pe + 6)[0]
    optsz   = struct.unpack_from("<H", buf, pe + 20)[0]
    opt     = pe + 24
    magic   = struct.unpack_from("<H", buf, opt)[0]
    dd      = opt + (96 if magic == 0x10b else 112)   # data directories offset
    imp_rva, imp_sz = struct.unpack_from("<II", buf, dd + 8)
    sec0 = opt + optsz
    sections = []
    for i in range(nsec):
        s = sec0 + 40 * i
        vs, va, rs, ra = struct.unpack_from("<I", buf, s + 8)[0], struct.unpack_from("<I", buf, s + 12)[0], \
                         struct.unpack_from("<I", buf, s + 16)[0], struct.unpack_from("<I", buf, s + 20)[0]
        sections.append((va, vs, ra, rs))
    out = []
    off = rva_to_off(imp_rva, sections)
    if off is None:
        return ("PE32+" if magic == 0x20b else "PE32"), machine, out
    while True:
        oft, ts, fc, name_rva, fthunk = struct.unpack_from("<IIIII", buf, off)
        if name_rva == 0 and oft == 0:
            break
        noff = rva_to_off(name_rva, sections)
        dll = cstr(buf, noff) if noff is not None else f"?rva{name_rva:#x}"
        funcs = []
        thunk = rva_to_off(oft if oft else fthunk, sections)
        if thunk is not None:
            while True:
                (val,) = struct.unpack_from("<I", buf, thunk)
                if val == 0:
                    break
                if val & 0x80000000:
                    funcs.append(f"ord{val & 0xFFFF}")
                else:
                    hn = rva_to_off(val & 0x7FFFFFFF, sections)
                    if hn is None:
                        funcs.append("?")
                    else:
                        try:
                            funcs.append(cstr(buf, hn + 2))
                        except Exception:
                            funcs.append("?")
                thunk += 4
        out.append((dll, funcs))
        off += 20
    return ("PE32+" if magic == 0x20b else "PE32"), machine, out

for path in sys.argv[1:]:
    kind, machine, tbl = imports(path)
    print(f"== {path}  [{kind}, machine 0x{machine:X}]")
    for dll, funcs in tbl:
        interesting = [f for f in funcs if any(k in f for k in ("Direct3D", "DirectDraw", "D3D", "Gl"))]
        print(f"  {dll}  ({len(funcs)} fns)" + (f"  [{', '.join(interesting[:6])}]" if interesting else ""))
