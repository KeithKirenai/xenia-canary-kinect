"""Parse Xenia's import dump into a name -> guest thunk address map.

Xenia's `UserModule` prints a full import listing at LogLevel::Info, e.g.:

    Imports:
     xam.xex - 334 imports
       ...
       F 820008A0 835AFA84 2E2 ( 738) !! LDIResetDecompression

The second column is the guest *thunk address* -- the target that guest `bl`
instructions branch to. Mapping name -> thunk is the prerequisite for any
offline reverse engineering: it turns a 20 MB .text into a call graph where
every kernel/XAM call site is identifiable.

    python tools/thunks.py build/bin/Windows/Release/xenia.log > thunks.txt
"""

import json
import re
import sys

# Xenia's listing is whitespace-delimited:
#     F 820008A0 835AFA84 2E2 ( 738) !! LDIResetDecompression
#     V 82000xxx          06C ( 108)    KeSomeDataVariable
# The trailing '!!' marks an unimplemented export, and the *last* token is
# always the name. Split on whitespace rather than regexing the tail: a
# pattern like "(\S+)?\s*(\S+)\s*$" backtracks through the name and silently
# splits "XeKeysGetConsoleID" into garbage.
ORD_TOKEN = re.compile(r"^\(\s*\d+\)$")


def parse(path):
    thunks = {}
    variables = {}
    lib = None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            if len(parts) >= 2 and parts[1] == "-" and parts[-1] == "imports":
                lib = parts[0]
                continue
            # The "( 738)" ordinal column has no padding space once the value
            # reaches four digits, so look for the '(' token by prefix rather
            # than by position.
            if parts[0] == "F" and len(parts) >= 6 and parts[4].startswith("("):
                thunks[parts[-1]] = {
                    "thunk": int(parts[2], 16),
                    "iat": int(parts[1], 16),
                    "ord": parts[3],
                    "impl": "!!" not in parts,
                    "lib": lib,
                }
            elif parts[0] == "V" and len(parts) >= 5 and parts[3].startswith("("):
                variables[parts[-1]] = {
                    "iat": int(parts[1], 16),
                    "ord": parts[2],
                    "impl": "!!" not in parts,
                    "lib": lib,
                }
    return thunks, variables


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    path = argv[1]
    thunks, variables = parse(path)
    if "--json" in argv:
        print(json.dumps({"thunks": thunks, "variables": variables}, indent=2))
        return 0
    unimplemented = [k for k, v in thunks.items() if not v["impl"]]
    for name in sorted(thunks):
        v = thunks[name]
        print("%08X %-6s %s%s" % (v["thunk"],
                                  "ok" if v["impl"] else "STUB",
                                  name,
                                  "" if v["impl"] else "   <-- UNIMPLEMENTED"))
    sys.stderr.write(
        "# %d thunks (%d unimplemented), %d variables, from %s\n"
        % (len(thunks), len(unimplemented), len(variables), path))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))