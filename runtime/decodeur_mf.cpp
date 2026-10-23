/* Décodeurs hôtes Media Foundation (Windows) : H.264 (Annex-B) -> NV12, AAC brut -> PCM 16 bits.
 * Utilisés par la réimplémentation native de movie::Decoder (vidéos du jeu). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "decodeur_mf.h"

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace {

bool g_mf_pret = false;
void mf_init() {
    if (g_mf_pret) return;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) g_mf_pret = true;
}

IMFTransform *creer_mft(const GUID &categorie, const GUID &majeur, const GUID &entree) {
    MFT_REGISTER_TYPE_INFO info{majeur, entree};
    IMFActivate **act = nullptr;
    UINT32 n = 0;
    MFTEnumEx(categorie, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &info, nullptr, &act, &n);
    IMFTransform *t = nullptr;
    for (UINT32 i = 0; i < n; i++) {
        if (!t) act[i]->ActivateObject(IID_PPV_ARGS(&t));
        act[i]->Release();
    }
    CoTaskMemFree(act);
    return t;
}

IMFSample *echantillon(const uint8_t *d, size_t n, int64_t pts_us) {
    IMFMediaBuffer *b = nullptr;
    if (FAILED(MFCreateMemoryBuffer((DWORD)n, &b))) return nullptr;
    BYTE *p;
    b->Lock(&p, nullptr, nullptr);
    memcpy(p, d, n);
    b->Unlock();
    b->SetCurrentLength((DWORD)n);
    IMFSample *s = nullptr;
    MFCreateSample(&s);
    s->AddBuffer(b);
    b->Release();
    s->SetSampleTime(pts_us * 10); /* unités de 100 ns */
    return s;
}

/* lit une sortie disponible ; renvoie false si le MFT réclame davantage d'entrée */
bool tirer_sortie(IMFTransform *t, std::vector<uint8_t> &donnees, int64_t &pts_us, bool &format_change) {
    format_change = false;
    MFT_OUTPUT_STREAM_INFO si{};
    t->GetOutputStreamInfo(0, &si);
    MFT_OUTPUT_DATA_BUFFER o{};
    IMFSample *s = nullptr;
    if (!(si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
        IMFMediaBuffer *b = nullptr;
        MFCreateMemoryBuffer(si.cbSize ? si.cbSize : (1 << 22), &b);
        MFCreateSample(&s);
        s->AddBuffer(b);
        b->Release();
        o.pSample = s;
    }
    DWORD statut = 0;
    HRESULT hr = t->ProcessOutput(0, 1, &o, &statut);
    if (o.pEvents) o.pEvents->Release();
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        format_change = true;
        if (s) s->Release();
        return false;
    }
    if (FAILED(hr)) {
        if (s) s->Release();
        return false;
    }
    IMFSample *r = o.pSample;
    LONGLONG t100 = 0;
    r->GetSampleTime(&t100);
    pts_us = t100 / 10;
    IMFMediaBuffer *b = nullptr;
    r->ConvertToContiguousBuffer(&b);
    BYTE *p;
    DWORD n = 0;
    b->Lock(&p, nullptr, &n);
    donnees.assign(p, p + n);
    b->Unlock();
    b->Release();
    r->Release();
    return true;
}

} // namespace

/* ------------------------------------------------------------------ vidéo */
bool DecodeurVideoMF::ouvrir() {
    mf_init();
    if (!g_mf_pret) return false;
    t = creer_mft(MFT_CATEGORY_VIDEO_DECODER, MFMediaType_Video, MFVideoFormat_H264);
    if (!t) { fprintf(stderr, "[vidéo] décodeur H.264 Media Foundation introuvable\n"); return false; }
    IMFMediaType *e = nullptr;
    MFCreateMediaType(&e);
    e->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    e->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    HRESULT hr = t->SetInputType(0, e, 0);
    e->Release();
    if (FAILED(hr)) return false;
    return choisir_sortie();
}

bool DecodeurVideoMF::choisir_sortie() {
    for (DWORD i = 0;; i++) {
        IMFMediaType *s = nullptr;
        if (FAILED(t->GetOutputAvailableType(0, i, &s))) return false;
        GUID g{};
        s->GetGUID(MF_MT_SUBTYPE, &g);
        if (g == MFVideoFormat_NV12) {
            t->SetOutputType(0, s, 0);
            UINT32 lw = 0, lh = 0;
            MFGetAttributeSize(s, MF_MT_FRAME_SIZE, &lw, &lh);
            largeur = lw; hauteur = lh;
            UINT32 pas_ = 0;
            pas = SUCCEEDED(s->GetUINT32(MF_MT_DEFAULT_STRIDE, &pas_)) ? (int)pas_ : (int)lw;
            MFVideoArea a{};
            if (SUCCEEDED(s->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&a, sizeof a, nullptr))) {
                visible_l = a.Area.cx; visible_h = a.Area.cy;
            } else {
                visible_l = lw; visible_h = lh;
            }
            s->Release();
            t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            return true;
        }
        s->Release();
    }
}

void DecodeurVideoMF::envoyer(const uint8_t *d, size_t n, int64_t pts_us) {
    if (!t || !n) return;
    IMFSample *s = echantillon(d, n, pts_us);
    if (!s) return;
    HRESULT hr = t->ProcessInput(0, s, 0);
    if (hr == MF_E_NOTACCEPTING) { /* sorties en attente : les récupérer puis réessayer */
        ImageNV12 img;
        while (recevoir(img)) attente.push_back(std::move(img));
        hr = t->ProcessInput(0, s, 0);
    }
    s->Release();
}

bool DecodeurVideoMF::recevoir(ImageNV12 &img) {
    if (!attente.empty()) {
        img = std::move(attente.front());
        attente.erase(attente.begin());
        return true;
    }
    if (!t) return false;
    for (int essai = 0; essai < 2; essai++) {
        bool change = false;
        int64_t pts = 0;
        if (tirer_sortie(t, img.donnees, pts, change)) {
            img.largeur = largeur; img.hauteur = hauteur; img.pas = pas; img.pts_us = pts;
            img.visible_l = visible_l; img.visible_h = visible_h;
            return true;
        }
        if (!change) return false;
        choisir_sortie(); /* nouveau format (dimensions connues après les premiers SPS) */
    }
    return false;
}

void DecodeurVideoMF::fin_de_flux() {
    if (t) { t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0); }
}
void DecodeurVideoMF::vider() {
    attente.clear();
    if (t) t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}
DecodeurVideoMF::~DecodeurVideoMF() {
    if (t) t->Release();
}

/* ------------------------------------------------------------------ audio */
bool DecodeurAudioMF::ouvrir(const uint8_t *asc, size_t n_asc, int frequence, int canaux) {
    mf_init();
    if (!g_mf_pret) return false;
    t = creer_mft(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio, MFAudioFormat_AAC);
    if (!t) { fprintf(stderr, "[audio] décodeur AAC Media Foundation introuvable\n"); return false; }
    IMFMediaType *e = nullptr;
    MFCreateMediaType(&e);
    e->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    e->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    e->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, frequence);
    e->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, canaux);
    e->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); /* trames AAC brutes */
    /* HEAACWAVEINFO (sans WAVEFORMATEX) : 12 octets puis AudioSpecificConfig */
    std::vector<uint8_t> ud(12, 0);
    ud.insert(ud.end(), asc, asc + n_asc);
    e->SetBlob(MF_MT_USER_DATA, ud.data(), (UINT32)ud.size());
    HRESULT hr = t->SetInputType(0, e, 0);
    e->Release();
    if (FAILED(hr)) { fprintf(stderr, "[audio] type d'entrée AAC refusé (%08lX)\n", hr); return false; }
    for (DWORD i = 0;; i++) {
        IMFMediaType *s = nullptr;
        if (FAILED(t->GetOutputAvailableType(0, i, &s))) return false;
        GUID g{};
        s->GetGUID(MF_MT_SUBTYPE, &g);
        UINT32 bits = 0;
        s->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        if (g == MFAudioFormat_PCM && bits == 16) {
            t->SetOutputType(0, s, 0);
            UINT32 f = 0, c = 0;
            s->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &f);
            s->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
            this->frequence = (int)f; this->canaux = (int)c;
            s->Release();
            t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            return true;
        }
        s->Release();
    }
}

void DecodeurAudioMF::envoyer(const uint8_t *d, size_t n, int64_t pts_us) {
    if (!t || !n) return;
    IMFSample *s = echantillon(d, n, pts_us);
    if (!s) return;
    if (t->ProcessInput(0, s, 0) == MF_E_NOTACCEPTING) {
        BlocPCM b;
        while (recevoir(b)) attente.push_back(std::move(b));
        t->ProcessInput(0, s, 0);
    }
    s->Release();
}

bool DecodeurAudioMF::recevoir(BlocPCM &b) {
    if (!attente.empty()) {
        b = std::move(attente.front());
        attente.erase(attente.begin());
        return true;
    }
    if (!t) return false;
    bool change = false;
    return tirer_sortie(t, b.donnees, b.pts_us, change);
}

void DecodeurAudioMF::vider() {
    attente.clear();
    if (t) t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
}
DecodeurAudioMF::~DecodeurAudioMF() {
    if (t) t->Release();
}
