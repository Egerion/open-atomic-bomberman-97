# Full-database Hex-Rays dump for BM95.EXE — decompiles EVERY function to
# pseudo.c next to the .idb (the BOMBRMAN folder). Facts source only; not
# committed to the repo.
#
# HOW TO RUN (the .py-as-IDC "Bad or ill-formed preprocessor command" error
# means IDAPython is not the active engine — fix that first):
#   1) If the bottom CLI can switch to Python, run there:
#        exec(open(r'<path-to-your-checkout>\docs\re\decompile_all.py', encoding='utf-8').read())
#   2) If Python isn't offered, run idapyswitch.exe from the IDA folder, pick
#      your Python 3, restart IDA, then use (1).
#   File > Script file also works once IDAPython is the registered .py handler.

import ida_hexrays, ida_funcs, idautils, idc

if not ida_hexrays.init_hexrays_plugin():
    print("Hex-Rays decompiler not available (need IDA with the decompiler).")
else:
    idb = idc.get_idb_path()
    out_path = idb.rsplit("\\", 1)[0].rsplit("/", 1)[0] + "/pseudo.c"
    funcs = sorted(idautils.Functions())
    total = len(funcs)
    ok = 0
    fail = 0
    with open(out_path, "w", encoding="utf-8", errors="replace") as out:
        out.write("// Full Hex-Rays dump of BM95.EXE — %d functions\n\n" % total)
        for idx, ea in enumerate(funcs):
            name = idc.get_func_name(ea)
            try:
                cf = ida_hexrays.decompile(ea)
                out.write("// ===== %08X  %s =====\n%s\n\n" % (ea, name, cf))
                ok += 1
            except Exception as e:
                out.write("// %08X  %s -- decompile failed: %s\n\n" % (ea, name, e))
                fail += 1
            if (idx + 1) % 200 == 0:
                print("... %d / %d" % (idx + 1, total))
    print("done: %d decompiled, %d failed -> %s" % (ok, fail, out_path))
