"""Génère les en-têtes SPIR-V des shaders internes de video_core (Eden) : équivalent de
src/video_core/host_shaders/CMakeLists.txt pour le moteur Vulkan seul (fichiers « opengl* » exclus).

Usage : python tools/eden/gen_shaders.py   (glslangValidator dans le PATH ; dépendances tierces : NX_TIERS)
Sortie : $NX_TIERS/eden_gen/include/video_core/host_shaders/<nom>_spv.h
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

TIERS = Path(os.environ.get("NX_TIERS") or Path(__file__).resolve().parents[2] / "tiers")
EDEN = TIERS / "eden"
SRC = EDEN / "src" / "video_core" / "host_shaders"
FFX = EDEN / "externals" / "FidelityFX-FSR" / "ffx-fsr"
SORTIE = TIERS / "eden_gen" / "include" / "video_core" / "host_shaders"
GLSLANG = shutil.which("glslangValidator") or sys.exit("glslangValidator introuvable (PATH)")
FLAGS = ["-V", "--quiet", f"-I{FFX}", "-DUseUniformBlock=1", "--target-env", "spirv1.3"]

# même liste que SHADER_FILES dans le CMakeLists d'Eden (lue depuis le fichier pour rester synchronisé)
cmake = (SRC / "CMakeLists.txt").read_text(encoding="utf-8")
debut = cmake.index("set(SHADER_FILES")
fin = cmake.index(")", debut)
fichiers = [l.strip().split("/")[-1] for l in cmake[debut:fin].splitlines() if "CMAKE_CURRENT_SOURCE_DIR" in l]
VARIANTES = [("convert_msaa_to_non_msaa.frag", "sint", "isampler2DMS", "ivec4"),
             ("convert_msaa_to_non_msaa.frag", "uint", "usampler2DMS", "uvec4"),
             ("convert_non_msaa_to_msaa.frag", "sint", "isampler2D", "ivec4"),
             ("convert_non_msaa_to_msaa.frag", "uint", "usampler2D", "uvec4")]

SORTIE.mkdir(parents=True, exist_ok=True)
erreurs = 0


def compiler(source, nom, extra=()):
    global erreurs
    sortie = SORTIE / f"{nom}_spv.h"
    cmd = [str(GLSLANG), *FLAGS, *extra, "--variable-name", f"{nom.upper()}_SPV", "-o", str(sortie), str(SRC / source)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        erreurs += 1
        print(f"ÉCHEC {source} : {r.stdout}{r.stderr}")


n = 0
for f in fichiers:
    if f.startswith("opengl") or f.endswith(".glsl"):
        continue
    compiler(f, f.replace(".", "_"))
    n += 1
for f, suffixe, sampler, texel in VARIANTES:
    base, ext = f.split(".")
    compiler(f, f"{base}_{suffixe}_{ext}", [f"-DSAMPLER_TYPE={sampler}", f"-DTEXEL_TYPE={texel}"])
    n += 1
print(f"{n} shaders, {erreurs} échec(s) -> {SORTIE}")
sys.exit(1 if erreurs else 0)
