"""Find guest call sites for imported kernel/XAM exports.

Combines the name->thunk map parsed out of Xenia's import dump with a scan of
every `bl` in the decompressed guest image, so a given API can be traced to
its callers offline -- no emulator run required.

    python tools/xrefs.py guest_nui_dump.bin thunks.txt XamUserNuiGetUserIndex
    python tools/xrefs.py guest_nui_dump.bin thunks.txt --all-nui
"""

import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from thunks import parse as parse_thunks          # noqa: E402
from xex import Image, iter_calls                 # noqa: E402


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 1
    dump, thunk_file, want = argv[1], argv[2], argv[3]
    thunks, _ = parse_thunks(thunk_file)
    img = Image(dump)

    if want == "--all-nui":
        names = [n for n in thunks if "nui" in n.lower() or "xstudio" in n.lower()]
    elif want == "--all":
        names = list(thunks)
    elif want == "--stubs":
        names = [n for n, v in thunks.items() if not v["impl"]]
    else:
        names = [want]

    # One pass over the image; bucket call sites by target thunk.
    wanted = {}
    for n in names:
        t = thunks.get(n)
        if t is None:
            print("!! %s is not in the import table" % n, file=sys.stderr)
            continue
        wanted.setdefault(t["thunk"], []).append(n)

    sites = {}
    for site, target in iter_calls(img):
        if target in wanted:
            sites.setdefault(target, []).append(site)

    total = 0
    for target in sorted(wanted):
        hits = sites.get(target, [])
        total += len(hits)
        for n in wanted[target]:
            mark = "" if thunks[n]["impl"] else "   <-- UNIMPLEMENTED in xenia"
            print("%s thunk=%08X  %d call site(s)%s"
                  % (n, target, len(hits), mark))
            for h in hits:
                print("      call from %#010x" % h)
    if not wanted:
        print("no matching imports", file=sys.stderr)
        return 1
    print("--- %d call sites across %d imports" % (total, len(wanted)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))