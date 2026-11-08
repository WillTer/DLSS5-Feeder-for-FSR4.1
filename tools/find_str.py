import glob

needles = [
    b"d\x00g\x00V\x00o\x00o\x00d\x00o\x00o\x00",  # UTF-16 "dgVoodoo"
    b"dgVoodoo DX",
    b"API Layer",
    b"dgVoodoo",
]
for f in glob.glob(r"C:/Program Files (x86)/Steam/steamapps/common/Spellforce Platinum Edition/D3D9.dll") + \
         glob.glob(r"C:/Program Files (x86)/Steam/steamapps/common/Spellforce Platinum Edition/DDraw.dll"):
    data = open(f, "rb").read()
    print("==", f.split("\\")[-1], len(data))
    for n in needles:
        start = 0
        while True:
            i = data.find(n, start)
            if i < 0:
                break
            print("  ", n[:12], hex(i), repr(data[max(0, i - 16): i + 44]))
            start = i + 1
