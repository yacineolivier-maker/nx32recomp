/* movie::Decoder natif (remplace le décodeur matériel NVDEC de la bibliothèque multimédia du SDK).
 * Protocole (rétro-ingénierie de la bibliothèque) : style MediaCodec — index d'entrée/sortie, événements
 * nn::os signalés {entrée dispo, sortie dispo, format changé, erreur}, statut 0x1000 = rien de disponible.
 * Décodage par Media Foundation (decodeur_mf.cpp) : H.264 -> NV12 serré w×h, AAC -> PCM s16 stéréo 48 kHz.
 * Le décodage est fait sur les threads du jeu (envoi d'entrée, libération de sortie) ; un index d'entrée n'est
 * rendu qu'une fois l'échantillon consommé, et pas plus de N_SORTIES images ne sont en circulation. */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include "cpu.h"
uint32_t natif_alloc(uint32_t taille, uint32_t align);
uint32_t natif_appel(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
uint32_t natif_appel6(Cpu *c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
uint32_t natif_vignette(const char *nom, void (*f)(Cpu *));
}
#include "decodeur_mf.h"
#include "jeu.h"

namespace {

/* fonctions et chaînes invitées de la bibliothèque multimédia, propres au jeu (jeu_adresses_movie) */
const JeuAdressesMovie *A = nullptr;

constexpr uint32_t ST_OK = 0, ST_RIEN = 0x1000, ST_PAS_CONFIGURE = 0x2002, ST_NON_PRIS = 0x4000, ST_TAMPON = 0x80000000u;
constexpr int N_ENTREES = 4, N_SORTIES = 8;
constexpr uint32_t FIN_DE_FLUX = 4;

struct Sortie {
    std::vector<uint8_t> donnees;
    int64_t pts = 0;
    uint32_t drapeaux = 0;
};

struct Decodeur {
    std::mutex m;
    uint32_t type = 0, format_sortie = 0;
    bool video = false, configure = false, demarre = false;
    uint32_t evenements = 0; /* DecoderEvents* : 4 x EventType* */
    uint32_t tmp = 0;        /* petite zone invitée de travail */
    int w = 1280, h = 720;
    std::vector<uint8_t> parametres; /* SPS/PPS en Annex-B (vidéo) */
    bool parametres_envoyes = false;
    std::unique_ptr<DecodeurVideoMF> dv;
    std::unique_ptr<DecodeurAudioMF> da;
    /* entrées : tampons invités */
    uint32_t tampon_entree[N_ENTREES] = {}, capacite_entree = 0;
    std::deque<int> entrees_libres;
    struct EntreeEnAttente { int idx; std::vector<uint8_t> d; int64_t pts; uint32_t drapeaux; };
    std::deque<EntreeEnAttente> entrees_en_attente;
    /* sorties */
    Sortie sorties[N_SORTIES];
    std::deque<int> sorties_libres, sorties_publiees;
    int sorties_tenues = 0;
    uint32_t derniere_erreur = 0;
};

std::mutex g_m;
std::map<uint32_t, std::shared_ptr<Decodeur>> g_decodeurs; /* id (objet+4) -> décodeur */
uint32_t g_prochain_id = 1, g_vtable = 0;

std::shared_ptr<Decodeur> dec(Cpu *c) {
    std::lock_guard<std::mutex> lk(g_m);
    auto it = g_decodeurs.find(rd32(c->r[0] + 4));
    return it == g_decodeurs.end() ? nullptr : it->second;
}
void signaler(Cpu *c, Decodeur &d, int k) {
    if (!d.evenements) return;
    uint32_t ev = rd32(d.evenements + 4 * k);
    if (ev) natif_appel(c, A->signal_event, ev, 0, 0, 0);
}

/* sortie vidéo : NV12 du MFT (pas, hauteur alignée) -> NV12 serré w×h */
std::vector<uint8_t> nv12_serre(const ImageNV12 &img, int w, int h) {
    std::vector<uint8_t> r((size_t)w * h + (size_t)(w / 2) * (h / 2) * 2);
    int pas = img.pas > 0 ? img.pas : img.largeur;
    size_t uv = (size_t)pas * img.hauteur;
    for (int y = 0; y < h && y < img.hauteur; y++)
        if ((size_t)(y + 1) * pas <= img.donnees.size()) memcpy(r.data() + (size_t)y * w, img.donnees.data() + (size_t)y * pas, w);
    uint8_t *ruv = r.data() + (size_t)w * h;
    for (int y = 0; y < h / 2; y++) {
        size_t s = uv + (size_t)y * pas;
        if (s + w <= img.donnees.size()) memcpy(ruv + (size_t)y * w, img.donnees.data() + s, (size_t)(w / 2) * 2);
    }
    return r;
}

/* récupère les sorties du MFT tant qu'il reste des emplacements ; renvoie le nombre publié */
int recolter(Decodeur &d) {
    int n = 0;
    while (!d.sorties_libres.empty()) {
        Sortie s;
        if (d.video) {
            ImageNV12 img;
            if (!d.dv || !d.dv->recevoir(img)) break;
            s.donnees = nv12_serre(img, d.w, d.h);
            s.pts = img.pts_us;
        } else {
            BlocPCM b;
            if (!d.da || !d.da->recevoir(b)) break;
            if (b.donnees.empty()) continue; /* jamais de sortie audio vide (elle bloquerait la file du jeu) */
            if (b.donnees.size() > 0x8000) b.donnees.resize(0x8000);
            s.donnees = std::move(b.donnees);
            s.pts = b.pts_us;
        }
        int i = d.sorties_libres.front();
        d.sorties_libres.pop_front();
        d.sorties[i] = std::move(s);
        d.sorties_publiees.push_back(i);
        n++;
    }
    return n;
}

/* consomme les entrées en attente tant que les sorties ne sont pas saturées ; signale les événements */
void pomper(Cpu *c, Decodeur &d) {
    int entrees_rendues = 0, sorties = 0;
    {
        std::lock_guard<std::mutex> lk(d.m);
        sorties += recolter(d);
        while (!d.entrees_en_attente.empty() && (int)d.sorties_publiees.size() + d.sorties_tenues < N_SORTIES - 2) {
            auto e = std::move(d.entrees_en_attente.front());
            d.entrees_en_attente.pop_front();
            if (!e.d.empty()) {
                if (d.video && d.dv) {
                    if (!d.parametres_envoyes && !d.parametres.empty()) {
                        std::vector<uint8_t> v = d.parametres;
                        v.insert(v.end(), e.d.begin(), e.d.end());
                        e.d.swap(v);
                        d.parametres_envoyes = true;
                    }
                    d.dv->envoyer(e.d.data(), e.d.size(), e.pts);
                } else if (d.da) {
                    d.da->envoyer(e.d.data(), e.d.size(), e.pts);
                }
            }
            if (e.drapeaux & FIN_DE_FLUX) {
                if (d.video && d.dv) d.dv->fin_de_flux();
                sorties += recolter(d);
                if (d.video && !d.sorties_libres.empty()) { /* sortie « fin de flux » (vidéo seulement) */
                    int i = d.sorties_libres.front();
                    d.sorties_libres.pop_front();
                    d.sorties[i] = Sortie{{}, e.pts, FIN_DE_FLUX};
                    d.sorties_publiees.push_back(i);
                    sorties++;
                }
            }
            d.entrees_libres.push_back(e.idx);
            entrees_rendues++;
            sorties += recolter(d);
        }
    }
    if (entrees_rendues) signaler(c, d, 0);
    if (sorties) signaler(c, d, 1);
}

/* ------------------------------------------------------------------ méthodes de la vtable */
void m_detruire(Cpu *c, bool liberer) {
    uint32_t self = c->r[0];
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_decodeurs.erase(rd32(self + 4));
    }
    wr32(self + 4, 0);
    if (liberer) natif_appel(c, A->operator_delete, self, 0, 0, 0);
    c->r[0] = self;
}
void m_d2(Cpu *c) { m_detruire(c, false); }
void m_d0(Cpu *c) { m_detruire(c, true); }

void m_configure(Cpu *c) { /* (self, MediaData*) */
    auto d = dec(c);
    uint32_t md = c->r[1];
    if (!d) { c->r[0] = ST_RIEN; return; }
    uint32_t t = d->tmp;
    if (natif_appel(c, A->md_find_int32, md, A->s_width, t, 0) & 0xFF) d->w = (int)rd32(t);
    if (natif_appel(c, A->md_find_int32, md, A->s_height, t, 0) & 0xFF) d->h = (int)rd32(t);
    bool ok = false;
    if (d->type == 1) { /* H.264 : SPS/PPS depuis « avcc » (octets profil/niveau ignorés) */
        d->video = true;
        if (natif_appel(c, A->md_find_buffer, md, A->s_avcc, t, 0) & 0xFF) {
            uint32_t impl = rd32(rd32(t));
            uint32_t base = rd32(impl), off = rd32(impl + 8), n = rd32(impl + 12);
            const uint8_t *a = g_mem + base + off;
            size_t p = 5;
            static const uint8_t debut[4] = {0, 0, 0, 1};
            for (int liste = 0; liste < 2 && p < n; liste++) {
                int nb = liste == 0 ? (a[p++] & 0x1F) : a[p++];
                for (int k = 0; k < nb && p + 2 <= n; k++) {
                    size_t l = (size_t)a[p] << 8 | a[p + 1];
                    p += 2;
                    if (p + l > n) break;
                    d->parametres.insert(d->parametres.end(), debut, debut + 4);
                    d->parametres.insert(d->parametres.end(), a + p, a + p + l);
                    p += l;
                }
            }
        }
        d->dv = std::make_unique<DecodeurVideoMF>();
        ok = d->dv->ouvrir();
        d->capacite_entree = 2 << 20;
    } else if (d->type == 4) { /* AAC : AudioSpecificConfig dans « esds » à l'octet 31 */
        std::vector<uint8_t> asc;
        if (natif_appel(c, A->md_find_buffer, md, A->s_esds, t, 0) & 0xFF) {
            uint32_t impl = rd32(rd32(t));
            uint32_t base = rd32(impl), off = rd32(impl + 8), n = rd32(impl + 12);
            if (n > 31) asc.assign(g_mem + base + off + 31, g_mem + base + off + n);
        }
        if (asc.empty()) asc = {0x11, 0x90};
        int frequence = 48000, canaux = 2;
        if (natif_appel(c, A->md_find_int32, md, A->s_sample_rate, t, 0) & 0xFF) frequence = (int)rd32(t);
        if (natif_appel(c, A->md_find_int32, md, A->s_channel_count, t, 0) & 0xFF) canaux = (int)rd32(t);
        d->da = std::make_unique<DecodeurAudioMF>();
        ok = d->da->ouvrir(asc.data(), asc.size(), frequence, canaux);
        d->capacite_entree = 64 << 10;
    }
    if (!ok) {
        fprintf(stderr, "[vidéo] Configure : type %u non pris en charge\n", d->type);
        d->derniere_erreur = ST_NON_PRIS;
        c->r[0] = ST_NON_PRIS;
        return;
    }
    for (int i = 0; i < N_ENTREES; i++)
        if (!d->tampon_entree[i]) d->tampon_entree[i] = natif_alloc(d->capacite_entree, 64);
    d->configure = true;
    fprintf(stderr, "[vidéo] Configure : %s %dx%d, %zu octets de paramètres\n", d->video ? "H.264" : "AAC", d->w, d->h, d->parametres.size());
    c->r[0] = ST_OK;
}

void m_register(Cpu *c) {
    auto d = dec(c);
    if (d) d->evenements = c->r[1];
    c->r[0] = d ? ST_OK : ST_RIEN;
}

void m_get_output_format(Cpu *c) { /* (self, MediaData*) */
    auto d = dec(c);
    uint32_t md = c->r[1];
    if (!d) { c->r[0] = ST_RIEN; return; }
    if (d->video) {
        natif_appel(c, A->md_set_string, md, A->s_mime, A->s_video_avc, 0);
        natif_appel(c, A->md_set_int32, md, A->s_width, (uint32_t)d->w, 0);
        natif_appel(c, A->md_set_int32, md, A->s_height, (uint32_t)d->h, 0);
        natif_appel(c, A->md_set_int32, md, A->s_stride, (uint32_t)d->w, 0);
        natif_appel6(c, A->md_set_rect, md, A->s_crop, 0, 0, (uint32_t)(d->w - 1), (uint32_t)(d->h - 1));
    } else {
        natif_appel(c, A->md_set_string, md, A->s_mime, A->s_audio_aac, 0);
        natif_appel(c, A->md_set_int32, md, A->s_sample_rate, 48000, 0);
        natif_appel(c, A->md_set_int32, md, A->s_channel_count, 2, 0);
    }
    c->r[0] = ST_OK;
}

void reinitialiser_files(Decodeur &d) {
    d.entrees_libres.clear();
    d.entrees_en_attente.clear();
    d.sorties_libres.clear();
    d.sorties_publiees.clear();
    d.sorties_tenues = 0;
    for (int i = 0; i < N_ENTREES; i++) d.entrees_libres.push_back(i);
    for (int i = 0; i < N_SORTIES; i++) d.sorties_libres.push_back(i);
}

void m_start(Cpu *c) {
    auto d = dec(c);
    if (!d) { c->r[0] = ST_RIEN; return; }
    if (!d->configure) { c->r[0] = ST_PAS_CONFIGURE; return; }
    {
        std::lock_guard<std::mutex> lk(d->m);
        reinitialiser_files(*d);
        d->demarre = true;
        d->parametres_envoyes = false;
    }
    if (d->video) signaler(c, *d, 2);
    signaler(c, *d, 0);
    c->r[0] = ST_OK;
}

void m_stop(Cpu *c) {
    auto d = dec(c);
    if (d) {
        std::lock_guard<std::mutex> lk(d->m);
        d->demarre = false;
        d->entrees_en_attente.clear();
        if (d->dv) d->dv->vider();
        if (d->da) d->da->vider();
    }
    c->r[0] = d ? ST_OK : ST_RIEN;
}
void m_flush(Cpu *c) {
    auto d = dec(c);
    if (d) {
        std::lock_guard<std::mutex> lk(d->m);
        if (d->dv) d->dv->vider();
        if (d->da) d->da->vider();
        reinitialiser_files(*d);
        d->parametres_envoyes = false;
    }
    c->r[0] = d ? ST_OK : ST_RIEN;
}
void m_reset(Cpu *c) {
    m_stop(c);
    auto d = dec(c);
    if (d) d->configure = false;
}

void m_acquire_input(Cpu *c) { /* (self, int *idx, int *restant) */
    auto d = dec(c);
    uint32_t pi = c->r[1], pr = c->r[2];
    if (!d) { c->r[0] = ST_RIEN; return; }
    std::lock_guard<std::mutex> lk(d->m);
    if (d->entrees_libres.empty()) {
        if (pi) wr32(pi, (uint32_t)-1);
        if (pr) wr32(pr, 0);
        c->r[0] = ST_RIEN;
        return;
    }
    int i = d->entrees_libres.front();
    d->entrees_libres.pop_front();
    if (pi) wr32(pi, (uint32_t)i);
    if (pr) wr32(pr, (uint32_t)d->entrees_libres.size());
    c->r[0] = ST_OK;
}

void m_get_input_buffer(Cpu *c) { /* (self, idx, Buffer*) */
    auto d = dec(c);
    int i = (int)c->r[1];
    uint32_t impl = c->r[2] ? rd32(c->r[2]) : 0;
    if (!d || i < 0 || i >= N_ENTREES || !impl) { c->r[0] = ST_TAMPON; return; }
    wr32(impl, d->tampon_entree[i]);
    wr32(impl + 4, d->capacite_entree);
    wr32(impl + 8, 0);
    wr32(impl + 12, d->capacite_entree);
    c->r[0] = ST_OK;
}

void m_send_input(Cpu *c) { /* (self, idx, décalage, taille, i64 pts (pile0-1), drapeaux (pile2)) */
    auto d = dec(c);
    int i = (int)c->r[1];
    uint32_t off = c->r[2], n = c->r[3];
    uint32_t sp = c->r[13];
    int64_t pts = (int64_t)((uint64_t)rd32(sp) | (uint64_t)rd32(sp + 4) << 32);
    uint32_t drapeaux = rd32(sp + 8);
    if (!d || i < 0 || i >= N_ENTREES) { c->r[0] = ST_TAMPON; return; }
    {
        std::lock_guard<std::mutex> lk(d->m);
        Decodeur::EntreeEnAttente e{i, {}, pts, drapeaux};
        if (n && off + n <= d->capacite_entree) e.d.assign(g_mem + d->tampon_entree[i] + off, g_mem + d->tampon_entree[i] + off + n);
        d->entrees_en_attente.push_back(std::move(e));
    }
    pomper(c, *d);
    c->r[0] = ST_OK;
}

void m_acquire_output(Cpu *c) { /* (self, int *idx, i64 *pts, u32 *drapeaux, int *restant) : 4e arg sur la pile */
    auto d = dec(c);
    uint32_t pi = c->r[1], pp = c->r[2], pd = c->r[3], pr = rd32(c->r[13]);
    if (!d) { c->r[0] = ST_RIEN; return; }
    std::lock_guard<std::mutex> lk(d->m);
    if (d->sorties_publiees.empty()) {
        if (pi) wr32(pi, (uint32_t)-1);
        if (pp) { wr32(pp, 0); wr32(pp + 4, 0); }
        if (pd) wr32(pd, 0);
        if (pr) wr32(pr, 0);
        c->r[0] = ST_RIEN;
        return;
    }
    int i = d->sorties_publiees.front();
    d->sorties_publiees.pop_front();
    d->sorties_tenues++;
    if (pi) wr32(pi, (uint32_t)i);
    if (pp) { wr32(pp, (uint32_t)d->sorties[i].pts); wr32(pp + 4, (uint32_t)((uint64_t)d->sorties[i].pts >> 32)); }
    if (pd) wr32(pd, d->sorties[i].drapeaux);
    if (pr) wr32(pr, (uint32_t)d->sorties_publiees.size());
    c->r[0] = ST_OK;
}

void m_get_output_buffer(Cpu *c) { /* (self, idx, Buffer*) : copie de l'image dans le tampon du client */
    auto d = dec(c);
    int i = (int)c->r[1];
    uint32_t impl = c->r[2] ? rd32(c->r[2]) : 0;
    if (!d || i < 0 || i >= N_SORTIES || !impl) { c->r[0] = ST_TAMPON; return; }
    std::lock_guard<std::mutex> lk(d->m);
    uint32_t base = rd32(impl), cap = rd32(impl + 4);
    const auto &s = d->sorties[i].donnees;
    uint32_t n = (uint32_t)std::min<size_t>(s.size(), cap);
    if (base && n) memcpy(g_mem + base, s.data(), n);
    static int nvid = 0;
    if (d->video && ++nvid == 90 && getenv("NX_VIDEO_VIDAGE")) /* diagnostic : 90e image NV12 serrée */
        if (FILE *f = fopen("build/video.nv12", "wb")) { fwrite(s.data(), 1, s.size(), f); fclose(f); }
    static int nlog = 0;
    if (d->video && nlog++ < 6) fprintf(stderr, "[vidéo] sortie %d -> %08X (%u octets, capacité %u ; UV serré à %08X)\n", i, base, n, cap, base + d->w * d->h);
    wr32(impl + 8, 0);
    wr32(impl + 12, n);
    c->r[0] = ST_OK;
}

void m_get_output_buffer_size(Cpu *c) { /* (self, int *taille, int *alignement) */
    auto d = dec(c);
    if (c->r[2]) wr32(c->r[2], 0x20);
    if (c->r[1] && d) wr32(c->r[1], d->video ? (uint32_t)(d->w * d->h + (d->w / 2) * (d->h / 2) * 2 + 0x40) : 0x8020u);
    c->r[0] = ST_OK;
}

void m_release_output(Cpu *c) { /* (self, idx) */
    auto d = dec(c);
    int i = (int)c->r[1];
    if (!d || i < 0 || i >= N_SORTIES) { c->r[0] = ST_TAMPON; return; }
    {
        std::lock_guard<std::mutex> lk(d->m);
        if (d->sorties_tenues > 0) d->sorties_tenues--;
        d->sorties[i].donnees.clear();
        d->sorties_libres.push_back(i);
    }
    pomper(c, *d);
    c->r[0] = ST_OK;
}

void m_get_last_error(Cpu *c) {
    auto d = dec(c);
    c->r[0] = d ? d->derniere_erreur : 0;
}

uint32_t vtable() {
    std::lock_guard<std::mutex> lk(g_m);
    if (g_vtable) return g_vtable;
    static const struct { const char *nom; void (*f)(Cpu *); } t[] = {
        {"movie::Decoder::~Decoder", m_d2},
        {"movie::Decoder::~Decoder(delete)", m_d0},
        {"movie::Decoder::Configure", m_configure},
        {"movie::Decoder::RegisterClientEvents", m_register},
        {"movie::Decoder::GetOutputFormat", m_get_output_format},
        {"movie::Decoder::Start", m_start},
        {"movie::Decoder::Stop", m_stop},
        {"movie::Decoder::Reset", m_reset},
        {"movie::Decoder::Flush", m_flush},
        {"movie::Decoder::AcquireInputBufferIndex", m_acquire_input},
        {"movie::Decoder::GetInputBuffer", m_get_input_buffer},
        {"movie::Decoder::SendInputBufferForDecode", m_send_input},
        {"movie::Decoder::AcquireOutputBufferIndex", m_acquire_output},
        {"movie::Decoder::GetOutputBuffer", m_get_output_buffer},
        {"movie::Decoder::GetOutputBufferSize", m_get_output_buffer_size},
        {"movie::Decoder::ReleaseOutputBufferIndex", m_release_output},
        {"movie::Decoder::GetLastError", m_get_last_error},
    };
    uint32_t vt = natif_alloc(8 + 4 * 17, 16) + 8; /* {décalage, typeinfo} puis les entrées */
    for (int i = 0; i < 17; i++) wr32(vt + 4 * i, natif_vignette(t[i].nom, t[i].f));
    g_vtable = vt;
    return vt;
}

} // namespace

/* movie::Decoder::Decoder(type, format de sortie) — remplace le constructeur (hle.txt) ; appelé seulement si
 * jeu_adresses_movie() n'est pas NULL (runtime/hle.c) */
extern "C" void movie_decoder_construire(Cpu *c) {
    A = jeu_adresses_movie();
    uint32_t self = c->r[0];
    auto d = std::make_shared<Decodeur>();
    d->type = c->r[1];
    d->format_sortie = c->r[2];
    d->tmp = natif_alloc(64, 16);
    uint32_t vt = vtable();
    uint32_t id;
    {
        std::lock_guard<std::mutex> lk(g_m);
        id = g_prochain_id++;
        g_decodeurs[id] = d;
    }
    wr32(self, vt);
    wr32(self + 4, id);
    fprintf(stderr, "[vidéo] movie::Decoder natif (type %u, sortie %u)\n", d->type, d->format_sortie);
    c->r[0] = self;
}
