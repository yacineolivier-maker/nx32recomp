"""Génère build/gen/entry.h à partir de build/lien/layout.json (bornes de l'image, symboles de démarrage,
constructeurs de chaque module)."""
import json
import sys
from pathlib import Path

DEMARRAGE = ["nnMain", "nninitStartup", "__nnDetailInitLibc0", "__nnDetailInitLibc1", "__nnDetailInitLibc2",
             "nnosInitialize", "nndiagStartup", "nninitInitializeSdkModule", "nninitFinalizeSdkModule",
             "nnosQuickExit"]

build = Path(sys.argv[1])
L = json.load(open(build / "lien" / "layout.json"))
out = ["#pragma once", "#include <stdint.h>", f"#define IMAGE_BASE 0x{L['base']:08X}u", f"#define IMAGE_END 0x{L['fin']:08X}u"]
for s in DEMARRAGE:
    if s in L["symboles"]:
        out.append(f"#define SYM_{s} 0x{L['symboles'][s][0]:08X}u")
# Ordre des constructeurs : celui de la liste des modules de rtld (ordre de chargement), rtld exclu.
ordre = [m for m in L["modules"] if m["nom"] != "rtld"]
out.append("typedef struct { const char *nom; uint32_t init; uint32_t init_array; uint32_t init_count; } ModInit;")
lignes = []
for m in ordre:
    init = m["base"] + m["init"] if m["init"] is not None else 0
    arr = m["base"] + m["init_array"] if m["init_array"] is not None else 0
    lignes.append(f'  {{"{m["nom"]}", 0x{init:08X}u, 0x{arr:08X}u, {m["init_arraysz"] // 4}u}},')
out.append(f"#define MODULE_COUNT {len(ordre)}")
out.append("static const ModInit g_modules[MODULE_COUNT] = {")
out += lignes
out.append("};")
(build / "gen" / "entry.h").write_text("\n".join(out) + "\n")
print("\n".join(out))
