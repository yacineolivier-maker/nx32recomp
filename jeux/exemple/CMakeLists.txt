# Bibliothèque d'un jeu : implémente runtime/jeu.h. Copier ce dossier dans le projet du jeu (hors de ce dépôt) et
# passer son chemin à CMake : -DNX_JEU_DIR=<dossier>.
add_library(jeu STATIC jeu.cpp)
target_include_directories(jeu PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
set_target_properties(jeu PROPERTIES CXX_STANDARD 23)
