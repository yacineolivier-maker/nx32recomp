/* Formats NVN et disposition mémoire « block-linear » de Maxwell (taille de stockage des textures). */
#pragma once
#include <cstdint>
#include <cstring>

struct FormatInfo {
    uint8_t bpb;     /* octets par bloc (texel pour les formats non compressés) */
    uint8_t bw, bh;  /* dimensions du bloc en texels (1x1 si non compressé) */
};

/* NVNformat -> taille de bloc. La numérotation suit l'énumération NVN (vérifié : 0x25 = RGBA8, format de la fenêtre). */
inline FormatInfo nvn_format_info(uint32_t f) {
    switch (f) {
    case 1: case 2: case 3: case 4: case 49: return {1, 1, 1};                       /* R8*, STENCIL8 */
    case 5: case 6: case 7: case 8: case 9: case 13: case 14: case 15: case 16: case 50:
    case 57: case 58: case 59: case 60: return {2, 1, 1};                             /* R16*, RG8*, DEPTH16, 16 bits */
    case 10: case 11: case 12: case 17: case 18: case 19: case 20: case 21: case 25: case 26:
    case 27: case 28: case 37: case 38: case 39: case 40: case 51: case 52: case 53: case 55:
    case 56: case 61: case 62: case 63: case 64: return {4, 1, 1};                    /* 32 bits */
    case 22: case 23: case 24: case 29: case 30: case 31: case 32: case 33: case 41: case 42:
    case 43: case 44: case 45: case 54: return {8, 1, 1};                             /* 64 bits */
    case 34: case 35: case 36: case 46: case 47: case 48: return {16, 1, 1};          /* 96/128 bits */
    case 65: case 66: case 69: case 70: case 73: case 74: return {8, 4, 4};          /* DXT1, RGTC1 */
    case 67: case 68: case 71: case 72: case 75: case 76: case 77: case 78: case 79: case 80:
        return {16, 4, 4};                                                             /* DXT3/5, RGTC2, BPTC */
    }
    if (f >= 0x79 && f <= 0x9C) { /* ASTC (RGBA puis SRGB) : 16 octets par bloc */
        static const uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                             {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        uint32_t i = (f - 0x79) % 14;
        return {16, dims[i][0], dims[i][1]};
    }
    return {4, 1, 1};
}

/* Hauteur de bloc (en GOB de 8 lignes) choisie comme le pilote : plus petite puissance de 2 couvrant la hauteur, max 16 */
inline uint32_t block_height_gobs(uint32_t h_blocks) {
    uint32_t gobs = (h_blocks + 7) / 8, bh = 1;
    while (bh < gobs && bh < 16) bh <<= 1;
    return bh;
}

/* Profondeur de bloc (en GOB) des textures 3D : plus petite puissance de 2 couvrant la profondeur, max 32 */
inline uint32_t block_depth_gobs(uint32_t d) {
    uint32_t bd = 1;
    while (bd < d && bd < 32) bd <<= 1;
    return bd;
}

/* Taille de stockage block-linear (tous niveaux, toutes couches), alignée sur 512 octets par niveau */
/* bh_impose : hauteur de bloc du niveau 0 imposée par une disposition empaquetée (BNTX), 0 = règle du pilote */
inline uint64_t blocklinear_size(uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t layers, uint32_t samples,
                                 uint32_t bh_impose = 0) {
    FormatInfo fi = nvn_format_info(fmt);
    uint64_t total = 0;
    uint32_t bh0 = bh_impose ? bh_impose : block_height_gobs((h + fi.bh - 1) / fi.bh), bd0 = block_depth_gobs(d ? d : 1);
    for (uint32_t l = 0; l < (levels ? levels : 1); l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = d >> l ? d >> l : 1;
        uint32_t wb = (lw + fi.bw - 1) / fi.bw, hb = (lh + fi.bh - 1) / fi.bh;
        uint32_t bh = block_height_gobs(hb), bd = block_depth_gobs(ld);
        if (bh > bh0) bh = bh0;
        if (bd > bd0) bd = bd0;
        uint64_t row = ((uint64_t)wb * fi.bpb + 63) & ~63ull;         /* GOB : 64 octets de large */
        uint64_t rows = (hb + 8ull * bh - 1) / (8ull * bh) * (8ull * bh);
        uint64_t prof = (ld + bd - 1) / bd * bd;
        total += (row * rows * prof + 511) & ~511ull;
    }
    uint64_t couche = (total + (512ull * bh0 * bd0) - 1) / (512ull * bh0 * bd0) * (512ull * bh0 * bd0);
    return couche * (layers ? layers : 1) * (samples > 1 ? samples : 1);
}

/* Décalage d'un octet (x en octets, y en lignes de blocs, z) dans une surface block-linear :
 * GOB = 64 octets x 8 lignes (512 octets), blocs de bh GOB de haut et bd GOB de profondeur. */
inline uint64_t blocklinear_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_gobs, uint32_t h_lignes, uint32_t bh, uint32_t bd) {
    const uint64_t gob_in = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
    const uint32_t hauteur_bloc = 8 * bh;                                  /* lignes par bloc */
    const uint32_t blocs_y = (h_lignes + hauteur_bloc - 1) / hauteur_bloc;
    const uint64_t taille_bloc = 512ull * bh * bd;
    const uint64_t bloc = ((uint64_t)(z / bd) * blocs_y + y / hauteur_bloc) * row_gobs + x / 64;
    const uint64_t gob = (uint64_t)(z % bd) * bh + (y % hauteur_bloc) / 8;
    return bloc * taille_bloc + gob * 512 + gob_in;
}

/* Disposition d'un niveau de mip dans le stockage block-linear (même règle que blocklinear_size) */
struct NiveauBL {
    uint64_t decalage;          /* depuis le début de la couche */
    uint32_t wb, hb, d;         /* dimensions en blocs */
    uint32_t bh;                /* hauteur de bloc (GOB) */
    uint32_t bd;                /* profondeur de bloc (GOB, textures 3D) */
    uint32_t row_gobs;
};
inline NiveauBL blocklinear_niveau(uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t niveau, uint32_t bh_impose = 0) {
    FormatInfo fi = nvn_format_info(fmt);
    uint32_t bh0 = bh_impose ? bh_impose : block_height_gobs((h + fi.bh - 1) / fi.bh), bd0 = block_depth_gobs(d ? d : 1);
    uint64_t dec = 0;
    NiveauBL r{};
    for (uint32_t l = 0; l <= niveau; l++) {
        uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = d >> l ? d >> l : 1;
        uint32_t wb = (lw + fi.bw - 1) / fi.bw, hb = (lh + fi.bh - 1) / fi.bh;
        uint32_t bh = block_height_gobs(hb), bd = block_depth_gobs(ld);
        if (bh > bh0) bh = bh0;
        if (bd > bd0) bd = bd0;
        uint64_t row = ((uint64_t)wb * fi.bpb + 63) & ~63ull;
        uint64_t rows = (hb + 8ull * bh - 1) / (8ull * bh) * (8ull * bh);
        if (l == niveau) { r = {dec, wb, hb, ld, bh, bd, (uint32_t)(row / 64)}; break; }
        dec += (row * rows * ((ld + bd - 1) / bd * bd) + 511) & ~511ull;
    }
    return r;
}

/* Déswizzle d'un niveau 2D (ou d'une tranche) : dst linéaire serré (wb * bpb octets par ligne) */
inline void blocklinear_vers_lineaire(const uint8_t *src, uint64_t src_taille, uint8_t *dst, uint32_t fmt, const NiveauBL &n, uint32_t z = 0) {
    const uint32_t bpb = nvn_format_info(fmt).bpb;
    const uint32_t ligne = n.wb * bpb;
    for (uint32_t y = 0; y < n.hb; y++) {
        uint8_t *o = dst + (uint64_t)y * ligne;
        for (uint32_t x = 0; x < ligne; x += 16) { /* 16 octets contigus dans un GOB */
            uint64_t a = blocklinear_offset(x, y, z, n.row_gobs, n.hb, n.bh, n.bd ? n.bd : 1);
            uint32_t k = ligne - x < 16 ? ligne - x : 16;
            if (a + k <= src_taille) memcpy(o + x, src + a, k);
        }
    }
}
