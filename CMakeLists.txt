cmake_minimum_required(VERSION 3.25)
project(nx32recomp C CXX)

# Exécutable d'un jeu = code recompilé (NX_BUILD/gen, généré par tools/recomp/gen.py) + runtime + bibliothèque du jeu
# (cible « jeu » du dossier NX_JEU_DIR, cf. runtime/jeu.h et jeux/exemple).
set(NX_JEU_DIR "${CMAKE_SOURCE_DIR}/jeux/exemple" CACHE PATH "Projet du jeu : CMakeLists.txt définissant la cible « jeu »")
set(NX_EXE "jeu" CACHE STRING "Nom de l'exécutable (le fichier de configuration s'appelle <NX_EXE>.ini)")
set(NX_BUILD "${CMAKE_SOURCE_DIR}/build" CACHE PATH "Dossier des sorties de tools/ (lien/, gen/)")
set(NX_TIERS "${CMAKE_SOURCE_DIR}/tiers" CACHE PATH "Dépendances tierces (cf. docs/DEPENDANCES.md)")

set(GEN_DIR ${NX_BUILD}/gen)
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${GEN_DIR}/rc_*.c)

add_library(recomp OBJECT ${GEN_SOURCES} ${GEN_DIR}/table.c)
target_include_directories(recomp PRIVATE ${GEN_DIR} ${CMAKE_SOURCE_DIR}/runtime)
# Code généré : sémantique ARM stricte (pas de contraction FMA, débordements signés définis, pas d'aliasing strict)
target_compile_options(recomp PRIVATE -O1 -fno-strict-aliasing -ffp-contract=off -fwrapv -Wno-everything)

add_subdirectory(${NX_JEU_DIR} jeu)

add_compile_definitions(RC_TRACE)  # trace des entrées de fonctions (développement)
add_executable(${NX_EXE}
    runtime/rt.c runtime/svc.c runtime/hle.c runtime/natif.c runtime/vignettes.cpp runtime/profil.c
    runtime/ipc.cpp runtime/services.cpp runtime/fs.cpp runtime/mods.cpp runtime/config.cpp
    runtime/fenetre.cpp runtime/vi.cpp runtime/eden_lien.cpp runtime/nvdrv.cpp runtime/nvdrv_dev.cpp
    runtime/hid.cpp runtime/manettes.cpp runtime/applets.cpp runtime/account.cpp runtime/settings.cpp runtime/time.cpp
    runtime/mii.cpp runtime/audio.cpp runtime/mixeur.cpp runtime/pl.cpp runtime/decodeur_mf.cpp runtime/movie.cpp
    $<TARGET_OBJECTS:recomp>)
target_include_directories(${NX_EXE} PRIVATE ${GEN_DIR} ${CMAKE_SOURCE_DIR}/runtime)
target_compile_options(${NX_EXE} PRIVATE -g -Wno-deprecated-volatile -O2 -fno-strict-aliasing -ffp-contract=off -fwrapv
                       -D_CRT_SECURE_NO_WARNINGS)
set_target_properties(${NX_EXE} PROPERTIES CXX_STANDARD 23)
target_link_options(${NX_EXE} PRIVATE -Wl,/STACK:268435456 -Wl,/DEBUG)
target_link_libraries(${NX_EXE} PRIVATE jeu dbghelp)

# SDL3 : fenêtre, manettes, audio
set(SDL3_DIR ${NX_TIERS}/SDL3-3.4.18)
target_include_directories(${NX_EXE} PRIVATE ${SDL3_DIR}/include)
target_link_libraries(${NX_EXE} PRIVATE ${SDL3_DIR}/lib/x64/SDL3.lib)
add_custom_command(TARGET ${NX_EXE} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                   ${SDL3_DIR}/lib/x64/SDL3.dll $<TARGET_FILE_DIR:${NX_EXE}>)

# émulation GPU d'Eden : exécute les commandes du pilote NVN d'origine du jeu (eden_gpu.dll, chargée en différé)
add_subdirectory(eden_gpu)
target_link_libraries(${NX_EXE} PRIVATE eden_gpu delayimp)
target_link_options(${NX_EXE} PRIVATE -Wl,/DELAYLOAD:eden_gpu.dll)
