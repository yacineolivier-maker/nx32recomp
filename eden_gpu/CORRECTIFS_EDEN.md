# Correctifs locaux appliqués à la copie d'Eden ($NX_TIERS/eden, non versionnée)

1. `src/shader_recompiler/frontend/maxwell/translate_program.cpp`, `CollectInterpolationInfo` :
   `throw NotImplementedException("Per component interpolation");` remplacé par `continue;`
   (l'interpolation de la première composante utilisée s'applique à tout l'attribut ; observé sur plusieurs programmes d'un jeu).
