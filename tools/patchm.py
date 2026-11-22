"""Patch missing libc++ undefined symbols in an Android ELF .so.

Usage:  python patchm.py <target.so> <libcxx_reference.so> [out.so] [--map old=new ...]

Rebuilds the .dynstr section, shifting strings and fixing every st_name offset
in .dynsym, then writes the result.  Missing symbols are mapped to "abort"
(which bionic libc always provides) unless an explicit --map is given.
"""
import struct
import sys


class Elf:
    def __init__(self, path):
        self.path = path
        self.d = bytearray(open(path, "rb").read())
        assert self.d[:4] == b"\x7fELF", "not ELF"
        assert self.d[4] == 2, "only ELF64 supported"
        self.e_shoff = struct.unpack_from("<Q", self.d, 0x28)[0]
        self.e_shentsize = struct.unpack_from("<H", self.d, 0x3A)[0]
        self.e_shnum = struct.unpack_from("<H", self.d, 0x3C)[0]
        self.sections = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            (name, typ, flags, addr, off, size, link, info, align,
             entsize) = struct.unpack_from("<IIQQQQIIQQ", self.d, o)
            self.sections.append(dict(name=name, typ=typ, off=off, size=size,
                                      link=link, entsize=entsize, idx=i))

    def find(self, typ, entsize=None):
        for s in self.sections:
            if s["typ"] == typ and (entsize is None or s["entsize"] == entsize):
                return s
        return None

    def strtab(self, sec):
        return self.d[self.sections[sec["link"]]["off"]:
                      self.sections[sec["link"]]["off"] +
                      self.sections[sec["link"]]["size"]]

    def dynsym_entries(self):
        sym = self.find(11)
        strat = self.strtab(sym)
        out = []
        for k in range(0, sym["size"], sym["entsize"]):
            off = sym["off"] + k
            st_name, st_info, st_other, st_shndx, st_value, st_size = \
                struct.unpack_from("<IBBHQQ", self.d, off)
            name = ""
            if st_name:
                end = strat.find(b"\0", st_name)
                name = strat[st_name:end].decode("latin1")
            out.append(dict(off=off, st_name=st_name, name=name,
                            shndx=st_shndx))
        return out

    def defined_names(self):
        return {e["name"] for e in self.dynsym_entries()
                if e["shndx"] != 0 and e["name"]}

    def undefined_names(self):
        return [e for e in self.dynsym_entries()
                if e["shndx"] == 0 and e["name"]]


def parse_strings(table):
    """Return list of (offset, string) for each null terminated entry."""
    out = []
    i = 0
    n = len(table)
    while i < n:
        end = table.find(b"\0", i)
        if end < 0:
            end = n
        out.append((i, table[i:end].decode("latin1")))
        i = end + 1
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    maps = {}
    for a in sys.argv[1:]:
        if a.startswith("--map"):
            for pair in a[5:].lstrip("=").split(","):
                if "=" in pair:
                    o, n = pair.split("=", 1)
                    maps[o] = n
    if len(args) < 2:
        print(__doc__)
        return 1
    target, ref = args[0], args[1]
    out_path = args[2] if len(args) > 2 else target.replace(".so", "_patched.so")

    t = Elf(target)
    r = Elf(ref)
    have = r.defined_names()

    undef = t.undefined_names()
    missing = []
    for e in undef:
        name = e["name"]
        if not (name.startswith("_ZNSt3__1") or name.startswith("_ZSt")
                or name.startswith("_ZNKSt3__1")):
            continue
        if name not in have:
            missing.append(name)

    print("missing libc++ symbols:", len(set(missing)))
    for m in sorted(set(missing)):
        print("   ", m)

    repl = {}
    for m in set(missing):
        repl[m] = maps.get(m, "abort")
    for old, new in maps.items():
        repl.setdefault(old, new)

    if not repl:
        print("nothing to patch")
        return 0

    dynstr = None
    for s in t.sections:
        if s["typ"] == 3:
            dynstr = s
            break
    assert dynstr, "no .dynstr"
    table = bytes(t.d[dynstr["off"]:dynstr["off"] + dynstr["size"]])
    entries = parse_strings(table)

    # build new table + offset map
    new_table = bytearray()
    off_map = {}
    for off, s in entries:
        off_map[off] = len(new_table)
        new = repl.get(s, s)
        new_table += new.encode("latin1") + b"\0"
    if len(new_table) > dynstr["size"]:
        print("ERROR: new table larger than original section")
        return 2

    # write table (zero padded)
    pad = dynstr["size"] - len(new_table)
    t.d[dynstr["off"]:dynstr["off"] + dynstr["size"]] = bytes(new_table) + b"\0" * pad

    # fix st_name in .dynsym
    fixed = 0
    for e in t.dynsym_entries():
        if e["st_name"] == 0:
            continue
        if e["st_name"] in off_map:
            new_off = off_map[e["st_name"]]
            if new_off != e["st_name"]:
                struct.pack_into("<I", t.d, e["off"], new_off)
                fixed += 1
        else:
            print("WARN: st_name not at string start:", e["st_name"])
    print("patched st_name entries:", fixed)
    print("replaced strings:", len([k for k in repl if k]))

    open(out_path, "wb").write(t.d)
    print("wrote", out_path)

    # verify
    v = Elf(out_path)
    still = [e["name"] for e in v.undefined_names()
             if (e["name"].startswith("_ZNSt3__1") or e["name"].startswith("_ZSt"))
             and e["name"] not in have]
    print("still missing:", len(still), still[:5])
    return 0


if __name__ == "__main__":
    sys.exit(main())
