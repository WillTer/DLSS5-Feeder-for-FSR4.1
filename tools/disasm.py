# Linear disassembly around an RVA in a PE, trying every start offset in a
# window so the instruction stream lines up exactly with the target address.
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

exe = sys.argv[1]
target = int(sys.argv[2], 16)          # RVA of interest (e.g. the return address)
back = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60
fwd = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x10

b = open(exe, "rb").read()
pe = struct.unpack_from("<I", b, 0x3C)[0]
nsec = struct.unpack_from("<H", b, pe + 6)[0]
optsz = struct.unpack_from("<H", b, pe + 20)[0]
imagebase = struct.unpack_from("<I", b, pe + 24 + 28)[0]
sec0 = pe + 24 + optsz
secs = []
for i in range(nsec):
    s = sec0 + 40 * i
    name = b[s:s+8].rstrip(b"\0").decode("latin1")
    vs, va, rs, ra = (struct.unpack_from("<I", b, s+8)[0], struct.unpack_from("<I", b, s+12)[0],
                      struct.unpack_from("<I", b, s+16)[0], struct.unpack_from("<I", b, s+20)[0])
    secs.append((name, va, vs, ra, rs))

def rva2off(rva):
    for name, va, vs, ra, rs in secs:
        if va <= rva < va + max(vs, rs):
            return ra + (rva - va), name
    return None, None

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False

start_rva = target - back
off, sec = rva2off(start_rva)
print(f"section {sec}, image base 0x{imagebase:X}, target RVA 0x{target:X}")

# try every alignment until the decode stream covers the target exactly
best = None
for delta in range(0, back):
    s_off = off + delta
    code = b[s_off: s_off + back + fwd]
    hits_target = False
    insns = []
    for insn in md.disasm(code, start_rva + delta):
        insns.append(insn)
        if insn.address + insn.size == target:
            hits_target = True
        if insn.address > target:
            break
    if hits_target and insns and insns[0].address <= target - 5:
        best = insns
        break

if best is None:
    print("no clean alignment found, raw bytes:")
    print(b[off:off + back + fwd].hex(" "))
else:
    for insn in best:
        mark = "  <== call site" if insn.address + insn.size == target else ""
        print(f"0x{insn.address:06X}: {insn.bytes.hex():<20} {insn.mnemonic} {insn.op_str}{mark}")
