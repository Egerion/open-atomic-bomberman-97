# Batch-decompile Atomic Bomberman's gameplay functions to pseudo.c
#
# Run this INSIDE IDA (with the Hex-Rays decompiler licensed/available):
#   File > Script file...  and pick this script.
# It writes `pseudo.c` next to the .idb (i.e. into the BOMBRMAN folder), which
# the reimplementation side then reads. Nothing here is committed to the repo;
# pseudo.c is a facts source, kept out of version control.
#
# Selection strategy (keeps the file to gameplay code, drops library noise):
#   - seed functions pinned via debug-string xrefs (movement/flame/bomb/disease)
#   - one hop of their callees and callers (the per-tick logic + direct helpers)
#   - any function that references a gameplay source-filename string
#   - only FLIRT-*unnamed* functions (sub_*) are emitted; Watcom runtime
#     functions are FLIRT-named and skipped.

import ida_hexrays, ida_funcs, idautils, idc, ida_xref

if not ida_hexrays.init_hexrays_plugin():
    print("Hex-Rays decompiler not available — need IDA with the decompiler.")
    raise SystemExit

# Functions located from embedded debug strings (see docs/re/facts.md).
SEEDS = [0x41f29b,  # player movement / animation-state ("godir is invalid")
         0x426d06,  # flame animation/update ("flame %s green")
         0x42331c,  # bomb animation ("bomb %s green")
         0x405de3,  # disease logic ("diseases_destroyable")
         0x40a1c6,  # offscreen/position check
         0x4124a4]  # getvalue(id)

GAME_C = {"bombs.c", "flame.c", "scheme.c", "misc.c", "graf.c", "ai.c",
          "campaign.c", "search.c", "options.c", "extra.c", "aliens.c"}

def func_start(ea):
    f = ida_funcs.get_func(ea)
    return f.start_ea if f else None

want = set(filter(None, (func_start(s) for s in SEEDS)))

# One hop of callees and callers around the seeds.
seed_funcs = set(want)
for ea in seed_funcs:
    for item in idautils.FuncItems(ea):
        for xr in idautils.XrefsFrom(item, 0):
            if xr.type in (ida_xref.fl_CN, ida_xref.fl_CF):   # call near/far
                fs = func_start(xr.to)
                if fs: want.add(fs)
    for xr in idautils.XrefsTo(ea):                            # callers
        fs = func_start(xr.frm)
        if fs: want.add(fs)

# Functions that reference a gameplay source-filename string.
for s in idautils.Strings():
    if str(s) in GAME_C:
        for xr in idautils.XrefsTo(s.ea):
            fs = func_start(xr.frm)
            if fs: want.add(fs)

out_path = idc.get_idb_path().rsplit("\\", 1)[0].rsplit("/", 1)[0] + "/pseudo.c"
n = 0
with open(out_path, "w") as out:
    for ea in sorted(want):
        name = idc.get_func_name(ea)
        # Emit gameplay code only; skip FLIRT-named Watcom runtime unless seeded.
        if not name.startswith("sub_") and ea not in set(filter(None, (func_start(s) for s in SEEDS))):
            continue
        try:
            cf = ida_hexrays.decompile(ea)
            out.write("// ===== %08X  %s =====\n%s\n\n" % (ea, name, cf))
            n += 1
        except Exception as e:
            out.write("// %08X  %s -- decompile failed: %s\n\n" % (ea, name, e))

print("wrote %s with %d functions" % (out_path, n))
