/* Mixeur audio natif pour audren (rendu audio du jeu) : voix (PCM16 / ADPCM) -> tampons de mix -> sous-mix ->
 * mix final -> sortie (« device sink ») -> SDL3 en stéréo 48 kHz.
 * Sémantique reprise d'audio_core d'Eden (VoiceInfo, MixContext, décodage ADPCM) ; effets (réverbération,
 * délai, aux) et filtres biquad ignorés pour l'instant. */
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
#include "cpu.h"
}
#include "mixeur.h"

namespace {

constexpr int MAX_TAMPONS_ONDE = 4, MAX_CANAUX = 6, MAX_TAMPONS_MIX = 24;

struct TamponOnde { /* copie serveur de WaveBufferInternal */
    uint64_t adresse = 0, taille = 0;
    int32_t debut = 0, fin = 0;
    bool boucle = false, fin_de_flux = false, envoye = true;
    uint64_t contexte = 0;
};

struct EtatCanal { /* VoiceState d'un canal */
    bool valide[MAX_TAMPONS_ONDE] = {};
    uint32_t index = 0, consommes = 0;
    int32_t decalage = 0;          /* échantillons lus dans le tampon courant */
    uint64_t joues = 0;
    double fraction = 0;           /* position fractionnaire du rééchantillonnage */
    int16_t prec[2] = {0, 0};      /* deux derniers échantillons (interpolation) */
    int16_t yn0 = 0, yn1 = 0;      /* historique ADPCM */
    uint16_t entete_adpcm = 0;
};

struct Voix {
    bool utilisee = false;
    uint8_t etat_jeu = 1;      /* PlayState : 0 démarré, 1 arrêté, 2 en pause */
    uint8_t format = 0;        /* SampleFormat : 2 PCM16, 6 ADPCM */
    uint32_t frequence = 48000, canaux = 1, mix = 0xFFFFFFFF;
    float hauteur = 1, volume = 1, volume_prec = 1;
    uint64_t donnees = 0, taille_donnees = 0; /* coefficients ADPCM */
    TamponOnde tampons[MAX_TAMPONS_ONDE];
    uint32_t index_tampon = 0;
    uint32_t ressources[MAX_CANAUX] = {};
    uint8_t vider = 0;
    EtatCanal etat[MAX_CANAUX];
    bool jouait = false;
};

struct Ressource { float volumes[MAX_TAMPONS_MIX] = {}; bool utilisee = false; };

struct Mix {
    bool utilise = false;
    float volume = 1;
    uint32_t nb_tampons = 0;
    int32_t destination = -1;
    float volumes[MAX_TAMPONS_MIX][MAX_TAMPONS_MIX] = {};
    int decalage = 0, distance = 0;
};

struct Sortie { bool utilisee = false; uint32_t nb = 0; int8_t entrees[MAX_CANAUX] = {}; };

} // namespace

struct Mixeur {
    std::mutex m;
    uint32_t frequence = 48000, echantillons = 240, nb_tampons_mix = 0;
    std::vector<Voix> voix;
    std::vector<Ressource> ressources;
    std::vector<Mix> mixes;
    std::vector<Sortie> sorties;
    std::vector<float> tampons; /* nb_tampons_mix x echantillons */
    SDL_AudioStream *flux = nullptr;
};

namespace {

/* ------------------------------------------------------------------ décodage d'un échantillon */
int16_t pcm16(const TamponOnde &t, uint32_t canaux, uint32_t c, int32_t pos) {
    uint64_t a = t.adresse + ((uint64_t)pos * canaux + c) * 2;
    if (a + 2 > t.adresse + t.taille || !t.adresse) return 0;
    return (int16_t)rd16((uint32_t)a);
}

/* ADPCM (DSP Nintendo) : trames de 8 octets = en-tête + 14 échantillons ; décodage séquentiel */
int16_t adpcm_suivant(const Voix &v, const TamponOnde &t, EtatCanal &e, int32_t pos) {
    uint32_t trame = (uint32_t)pos / 14, k = (uint32_t)pos % 14;
    uint64_t base = t.adresse + (uint64_t)trame * 8;
    if (base + 8 > t.adresse + t.taille || !t.adresse) return 0;
    uint8_t entete = rd8((uint32_t)base);
    uint8_t octet = rd8((uint32_t)(base + 1 + k / 2));
    int32_t code = (k & 1) ? (octet & 0xF) : (octet >> 4);
    if (code >= 8) code -= 16;
    int32_t echelle = entete & 0xF, ic = (entete >> 4) & 7;
    int32_t c0 = (int16_t)rd16((uint32_t)(v.donnees + ic * 4)), c1 = (int16_t)rd16((uint32_t)(v.donnees + ic * 4 + 2));
    int32_t xn = code * (1 << echelle);
    int32_t s = ((xn << 11) + 0x400 + c0 * e.yn0 + c1 * e.yn1) >> 11;
    s = std::clamp(s, -0x8000, 0x7FFF);
    e.yn1 = e.yn0;
    e.yn0 = (int16_t)s;
    return (int16_t)s;
}

/* lit l'échantillon suivant d'un canal de voix ; renvoie false quand plus rien n'est à jouer */
bool echantillon_suivant(Voix &v, uint32_t c, int16_t &out) {
    EtatCanal &e = v.etat[c];
    for (int essais = 0; essais < MAX_TAMPONS_ONDE + 1; essais++) {
        if (!e.valide[e.index]) return false;
        TamponOnde &t = v.tampons[e.index];
        int32_t pos = t.debut + e.decalage;
        if (pos < t.fin) {
            if (v.format == 6) {
                if (e.decalage == 0 && t.contexte) { /* contexte ADPCM fourni au début du tampon */
                    e.yn0 = (int16_t)rd16((uint32_t)t.contexte + 2);
                    e.yn1 = (int16_t)rd16((uint32_t)t.contexte + 4);
                }
                out = adpcm_suivant(v, t, e, pos);
            } else {
                out = pcm16(t, v.canaux, c, pos);
            }
            e.decalage++;
            e.joues++;
            return true;
        }
        /* fin du tampon */
        if (t.boucle) {
            e.decalage = 0;
            if (v.format == 6 && t.contexte) {
                e.yn0 = (int16_t)rd16((uint32_t)t.contexte + 2);
                e.yn1 = (int16_t)rd16((uint32_t)t.contexte + 4);
            }
            continue;
        }
        e.valide[e.index] = false;
        e.index = (e.index + 1) % MAX_TAMPONS_ONDE;
        e.consommes++;
        e.decalage = 0;
        if (t.fin_de_flux) { e.joues = 0; return false; }
    }
    return false;
}

void rendre_voix(Mixeur &mx, Voix &v) {
    if (!v.utilisee || v.etat_jeu != 0 || v.mix >= mx.mixes.size()) return;
    const Mix &dest = mx.mixes[v.mix];
    if (!dest.utilise) return;
    double pas = (double)v.frequence * std::max(0.0f, v.hauteur) / mx.frequence;
    for (uint32_t c = 0; c < v.canaux && c < MAX_CANAUX; c++) {
        EtatCanal &e = v.etat[c];
        const Ressource *r = v.ressources[c] < mx.ressources.size() ? &mx.ressources[v.ressources[c]] : nullptr;
        if (!r) continue;
        std::vector<float> s(mx.echantillons);
        bool actif = true;
        for (uint32_t i = 0; i < mx.echantillons; i++) {
            while (e.fraction >= 1.0) { /* données épuisées : zéros (jamais d'extrapolation) */
                int16_t x = 0;
                if (actif && !echantillon_suivant(v, c, x)) { actif = false; x = 0; }
                e.prec[0] = e.prec[1];
                e.prec[1] = x;
                e.fraction -= 1.0;
            }
            float a = e.prec[0], b = e.prec[1];
            s[i] = (float)(a + (b - a) * e.fraction) / 32768.0f;
            e.fraction += pas;
        }
        static const bool dbg = getenv("NX_DBG_AUDIO_VOIX") != nullptr;
        if (dbg) {
            float mx_ = 0;
            for (float x : s) mx_ = std::max(mx_, std::fabs(x));
            static int n = 0;
            if (mx_ > 0.95f && n++ < 40) {
                int vi = (int)(&v - mx.voix.data());
                const TamponOnde &t = v.tampons[e.index];
                fprintf(stderr, "[audio] voix %d canal %u fmt %u %u Hz hauteur %.3f vol %.3f->%.3f crête %.3f tampon %u @%llX %d..%d boucle %d dec %d actif %d\n", vi, c,
                        v.format, v.frequence, v.hauteur, v.volume_prec, v.volume, mx_, e.index, (unsigned long long)t.adresse, t.debut, t.fin,
                        t.boucle, e.decalage, actif);
            }
        }
        /* volume de la voix (rampe linéaire sur la trame) puis volumes de la ressource vers les tampons du mix */
        for (uint32_t j = 0; j < dest.nb_tampons && j < MAX_TAMPONS_MIX; j++) {
            float vol = r->volumes[j];
            if (vol == 0.0f) continue;
            float *t = &mx.tampons[(size_t)(dest.decalage + j) * mx.echantillons];
            for (uint32_t i = 0; i < mx.echantillons; i++) {
                float vv = v.volume_prec + (v.volume - v.volume_prec) * (float)i / mx.echantillons;
                t[i] += s[i] * vv * vol;
            }
        }
    }
    v.volume_prec = v.volume;
}

} // namespace

/* ------------------------------------------------------------------ interface */
Mixeur *mixeur_creer(uint32_t frequence, uint32_t echantillons, uint32_t nb_voix, uint32_t nb_mix, uint32_t nb_tampons_mix, uint32_t nb_sorties) {
    auto *mx = new Mixeur;
    mx->frequence = frequence ? frequence : 48000;
    mx->echantillons = echantillons ? echantillons : 240;
    mx->nb_tampons_mix = nb_tampons_mix;
    mx->voix.resize(nb_voix);
    mx->ressources.resize(nb_voix);
    mx->mixes.resize(nb_mix);
    mx->sorties.resize(nb_sorties);
    mx->tampons.assign((size_t)nb_tampons_mix * mx->echantillons, 0.0f);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, (int)mx->frequence};
    mx->flux = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (mx->flux) SDL_ResumeAudioStreamDevice(mx->flux);
    else fprintf(stderr, "[audio] sortie SDL indisponible : %s\n", SDL_GetError());
    return mx;
}

/* Lecture de RequestUpdate (révision 1 : voix 0x170, mix 0x930, sorties 0x140) ; écriture des états des voix */
void mixeur_mise_a_jour(Mixeur *mx, uint32_t entree, uint32_t sortie_voix) {
    std::lock_guard<std::mutex> lk(mx->m);
    uint32_t h = entree;
    uint32_t t_comport = rd32(h + 0x04), t_pools = rd32(h + 0x08), t_voix = rd32(h + 0x0C), t_ress = rd32(h + 0x10),
             t_effets = rd32(h + 0x14), t_mix = rd32(h + 0x18), t_sorties = rd32(h + 0x1C);
    uint32_t p = h + 0x40 + t_comport + t_pools;
    /* ressources de canaux (0x70) */
    for (uint32_t i = 0; i < t_ress / 0x70; i++) {
        uint32_t a = p + i * 0x70, id = rd32(a);
        if (id >= mx->ressources.size()) continue;
        Ressource &r = mx->ressources[id];
        r.utilisee = rd8(a + 0x64) != 0;
        for (int j = 0; j < MAX_TAMPONS_MIX; j++) r.volumes[j] = u2f(rd32(a + 4 + 4 * j));
    }
    p += t_ress;
    /* voix (0x170) */
    uint32_t nv = t_voix / 0x170;
    for (uint32_t i = 0; i < nv && i < mx->voix.size(); i++) {
        uint32_t a = p + i * 0x170;
        Voix &v = mx->voix[i];
        bool nouvelle = rd8(a + 0x08) != 0;
        v.utilisee = rd8(a + 0x09) != 0;
        if (!v.utilisee) continue;
        if (nouvelle) {
            v = Voix{};
            v.utilisee = true;
        }
        uint8_t etat = rd8(a + 0x0A);
        if (v.etat_jeu == 0 && etat == 1) { /* arrêt demandé : tampons rendus, état remis à zéro */
            for (uint32_t c = 0; c < MAX_CANAUX; c++) {
                EtatCanal &e = v.etat[c];
                for (int k = 0; k < MAX_TAMPONS_ONDE; k++)
                    if (e.valide[k]) { e.consommes++; e.valide[k] = false; }
                e.index = 0; e.decalage = 0; e.joues = 0; e.yn0 = e.yn1 = 0; e.fraction = 0; e.prec[0] = e.prec[1] = 0;
            }
            for (auto &t : v.tampons) t.envoye = true;
        }
        v.etat_jeu = etat;
        v.format = rd8(a + 0x0B);
        v.frequence = rd32(a + 0x0C);
        v.canaux = std::min<uint32_t>(rd32(a + 0x18), MAX_CANAUX);
        v.hauteur = u2f(rd32(a + 0x1C));
        v.volume = u2f(rd32(a + 0x20));
        if (nouvelle) v.volume_prec = v.volume;
        v.donnees = rd64(a + 0x48);
        v.taille_donnees = rd64(a + 0x50);
        v.mix = rd32(a + 0x58);
        v.index_tampon = rd16(a + 0x40);
        for (int c = 0; c < MAX_CANAUX; c++) v.ressources[c] = rd32(a + 0x140 + 4 * c);
        uint8_t vider = rd8(a + 0x159);
        for (int k = 0; k < MAX_TAMPONS_ONDE; k++) {
            uint32_t w = a + 0x60 + k * 0x38;
            if (rd8(w + 0x1A)) continue; /* déjà envoyé au DSP */
            TamponOnde &t = v.tampons[k];
            t.adresse = rd64(w);
            t.taille = rd64(w + 8);
            t.debut = (int32_t)rd32(w + 0x10);
            t.fin = (int32_t)rd32(w + 0x14);
            t.boucle = rd8(w + 0x18) != 0;
            t.fin_de_flux = rd8(w + 0x19) != 0;
            t.contexte = rd64(w + 0x20);
            t.envoye = false;
        }
        /* vidage demandé */
        for (uint32_t n = 0; n < vider; n++) {
            uint32_t k = (v.index_tampon + n) % MAX_TAMPONS_ONDE;
            v.tampons[k].envoye = true;
            for (uint32_t c = 0; c < v.canaux; c++) {
                EtatCanal &e = v.etat[c];
                if (e.index == k) { e.index = (e.index + 1) % MAX_TAMPONS_ONDE; e.consommes++; }
                e.valide[k] = false;
            }
        }
        /* démarré : les nouveaux tampons deviennent jouables */
        if (v.etat_jeu == 0)
            for (int k = 0; k < MAX_TAMPONS_ONDE; k++)
                if (!v.tampons[k].envoye) {
                    for (uint32_t c = 0; c < v.canaux; c++) v.etat[c].valide[k] = true;
                    v.tampons[k].envoye = true;
                }
    }
    /* états de sortie des voix (0x10) */
    for (uint32_t i = 0; i < nv && i < mx->voix.size(); i++) {
        const Voix &v = mx->voix[i];
        uint32_t o = sortie_voix + i * 0x10;
        wr64(o, v.etat[0].joues);
        wr32(o + 8, v.etat[0].consommes);
        wr32(o + 12, 0);
    }
    p += t_voix + t_effets;
    /* mix (0x930) */
    bool modifie = false;
    for (uint32_t i = 0; i < t_mix / 0x930 && i < mx->mixes.size(); i++) {
        uint32_t a = p + i * 0x930;
        Mix &x = mx->mixes[i];
        bool u = rd8(a + 0x0C) != 0;
        uint32_t nb = rd32(a + 0x08);
        int32_t d = (int32_t)rd32(a + 0x924);
        if (u != x.utilise || nb != x.nb_tampons || d != x.destination) modifie = true;
        x.utilise = u;
        x.volume = u2f(rd32(a));
        x.nb_tampons = std::min<uint32_t>(nb, MAX_TAMPONS_MIX);
        x.destination = d;
        for (int r = 0; r < MAX_TAMPONS_MIX; r++)
            for (int c = 0; c < MAX_TAMPONS_MIX; c++) x.volumes[r][c] = u2f(rd32(a + 0x24 + 4 * (r * MAX_TAMPONS_MIX + c)));
    }
    if (modifie) { /* décalages des tampons : du plus éloigné du mix final au mix final (comme MixContext) */
        for (auto &x : mx->mixes) {
            x.distance = 0;
            int32_t d = x.destination;
            for (int k = 0; k < 64 && d >= 0 && d < (int32_t)mx->mixes.size(); k++) { x.distance++; d = mx->mixes[d].destination; }
        }
        std::vector<int> ordre(mx->mixes.size());
        for (size_t i = 0; i < ordre.size(); i++) ordre[i] = (int)i;
        std::stable_sort(ordre.begin(), ordre.end(), [&](int a, int b) { return mx->mixes[a].distance > mx->mixes[b].distance; });
        int off = 0;
        for (int i : ordre)
            if (mx->mixes[i].utilise) { mx->mixes[i].decalage = off; off += mx->mixes[i].nb_tampons; }
        if ((uint32_t)off > mx->nb_tampons_mix) { mx->nb_tampons_mix = off; mx->tampons.assign((size_t)off * mx->echantillons, 0.0f); }
    }
    p += t_mix;
    /* sorties (0x140) : périphérique */
    for (uint32_t i = 0; i < t_sorties / 0x140 && i < mx->sorties.size(); i++) {
        uint32_t a = p + i * 0x140;
        Sortie &s = mx->sorties[i];
        s.utilisee = rd8(a + 1) != 0 && rd8(a) == 1; /* type 1 : périphérique */
        s.nb = std::min<uint32_t>(rd32(a + 0x20 + 0x100), MAX_CANAUX);
        for (int c = 0; c < MAX_CANAUX; c++) s.entrees[c] = (int8_t)rd8(a + 0x20 + 0x104 + c);
    }
}

/* Une trame audio : rendu des voix, mixage, sortie vers SDL */
void mixeur_trame(Mixeur *mx) {
    std::vector<float> stereo;
    {
        std::lock_guard<std::mutex> lk(mx->m);
        std::fill(mx->tampons.begin(), mx->tampons.end(), 0.0f);
        for (auto &v : mx->voix) rendre_voix(*mx, v);
        /* sous-mix -> destination, du plus éloigné au plus proche */
        std::vector<int> ordre;
        for (size_t i = 0; i < mx->mixes.size(); i++)
            if (mx->mixes[i].utilise && mx->mixes[i].destination >= 0) ordre.push_back((int)i);
        std::stable_sort(ordre.begin(), ordre.end(), [&](int a, int b) { return mx->mixes[a].distance > mx->mixes[b].distance; });
        const uint32_t n = mx->echantillons;
        for (int i : ordre) {
            const Mix &s = mx->mixes[i];
            if (s.destination >= (int32_t)mx->mixes.size()) continue;
            const Mix &d = mx->mixes[s.destination];
            for (uint32_t a = 0; a < s.nb_tampons; a++)
                for (uint32_t b = 0; b < d.nb_tampons; b++) {
                    float vol = s.volumes[a][b] * s.volume;
                    if (vol == 0.0f) continue;
                    const float *src = &mx->tampons[(size_t)(s.decalage + a) * n];
                    float *dst = &mx->tampons[(size_t)(d.decalage + b) * n];
                    for (uint32_t k = 0; k < n; k++) dst[k] += src[k] * vol;
                }
        }
        /* mix final (id 0) -> sortie périphérique -> stéréo */
        const Mix &f = mx->mixes.empty() ? Mix{} : mx->mixes[0];
        stereo.assign((size_t)n * 2, 0.0f);
        const Sortie *s = nullptr;
        for (auto &x : mx->sorties)
            if (x.utilisee) { s = &x; break; }
        auto canal = [&](int c) -> const float * {
            if (!s || c >= (int)s->nb) return nullptr;
            int b = s->entrees[c];
            if (b < 0 || (uint32_t)b >= mx->nb_tampons_mix) return nullptr;
            return &mx->tampons[(size_t)b * n];
        };
        const float *g = canal(0), *dr = canal(1), *ce = canal(2), *ar_g = canal(4), *ar_d = canal(5);
        for (uint32_t k = 0; k < n; k++) { /* 5.1 -> stéréo si nécessaire */
            float l = g ? g[k] : 0, r = dr ? dr[k] : 0;
            if (s && s->nb >= 6) {
                float c = ce ? ce[k] : 0;
                l += 0.707f * c + 0.707f * (ar_g ? ar_g[k] : 0);
                r += 0.707f * c + 0.707f * (ar_d ? ar_d[k] : 0);
            }
            stereo[2 * k] = std::clamp(l * f.volume, -1.0f, 1.0f);
            stereo[2 * k + 1] = std::clamp(r * f.volume, -1.0f, 1.0f);
        }
    }
    static FILE *wav = [] {
        const char *c = getenv("NX_AUDIO_WAV");
        FILE *f = c ? fopen(c, "wb") : nullptr;
        if (f) { uint8_t z[44] = {}; fwrite(z, 1, 44, f); }
        return f;
    }();
    if (wav) { /* diagnostic : PCM 16 bits stéréo, en-tête complété à chaque trame */
        static uint32_t octets = 0;
        std::vector<int16_t> pcm(stereo.size());
        for (size_t i = 0; i < stereo.size(); i++) pcm[i] = (int16_t)(stereo[i] * 32767.0f);
        fwrite(pcm.data(), 2, pcm.size(), wav);
        octets += (uint32_t)pcm.size() * 2;
        long pos = ftell(wav);
        uint32_t fr = mx->frequence, br = fr * 4, t36 = 36 + octets;
        uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0};
        memcpy(h + 4, &t36, 4);
        memcpy(h + 24, &fr, 4);
        memcpy(h + 28, &br, 4);
        h[32] = 4; h[34] = 16;
        memcpy(h + 36, "data", 4);
        memcpy(h + 40, &octets, 4);
        fseek(wav, 0, SEEK_SET);
        fwrite(h, 1, 44, wav);
        fseek(wav, pos, SEEK_SET);
    }
    if (mx->flux) {
        /* latence bornée : au-delà de 100 ms en file, on saute la trame */
        if (SDL_GetAudioStreamQueued(mx->flux) < (int)(mx->frequence * 2 * sizeof(float) / 10))
            SDL_PutAudioStreamData(mx->flux, stereo.data(), (int)(stereo.size() * sizeof(float)));
    }
}
