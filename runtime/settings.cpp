/* set / set:sys : langue et région ([systeme] de l'ini). Interfaces : https://switchbrew.org/wiki/Settings_services */
#include "ipc.h"

#include <cstdio>
#include <string>

std::string config_texte(const char *section, const char *cle, const char *defaut); /* config.cpp */

namespace {

/* ordre de nn::settings::Language */
const char *k_langues[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419"};
constexpr uint32_t k_nb = sizeof(k_langues) / sizeof(k_langues[0]);

uint64_t code(uint32_t i) {
    uint64_t v = 0;
    if (i < k_nb) memcpy(&v, k_langues[i], strlen(k_langues[i]));
    return v;
}

void ecrire_codes(Request &rq, Response &rs) {
    Buf b = rq.out();
    uint32_t n = b.size / 8 < k_nb ? b.size / 8 : k_nb;
    for (uint32_t i = 0; i < n; i++) wr64(b.addr + 8 * i, code(i));
    rs.push<int32_t>((int32_t)n);
}

/* [systeme] langue : code de nn::settings::Language (fr par défaut) */
uint32_t langue_systeme() {
    static const uint32_t i = [] {
        const std::string l = config_texte("systeme", "langue", "fr");
        for (uint32_t k = 0; k < k_nb; k++)
            if (l == k_langues[k]) return k;
        fprintf(stderr, "[set] langue %s inconnue : fr\n", l.c_str());
        return 2u;
    }();
    return i;
}

/* [systeme] region : japon, usa, europe, australie, chine, coree, taiwan */
uint32_t region_systeme() {
    static const uint32_t r = [] {
        const char *noms[] = {"japon", "usa", "europe", "australie", "chine", "coree", "taiwan"};
        const std::string v = config_texte("systeme", "region", "europe");
        for (uint32_t k = 0; k < 7; k++)
            if (v == noms[k]) return k;
        return 2u;
    }();
    return r;
}

} // namespace

/* code de la langue du système (appletOE GetDesiredLanguage) */
uint64_t langue_code_systeme() { return code(langue_systeme()); }

void settings_register() {
    ipc_register("set", [] {
        auto s = std::make_shared<TableService>("set");
        s->cmds[0] = [](Request &, Response &rs) { rs.push<uint64_t>(code(langue_systeme())); }; /* GetLanguageCode */
        s->cmds[1] = ecrire_codes;                                                               /* GetAvailableLanguageCodes */
        s->cmds[2] = [](Request &rq, Response &rs) { rs.push<uint64_t>(code(rq.pop<uint32_t>())); }; /* MakeLanguageCode */
        s->cmds[3] = [](Request &, Response &rs) { rs.push<int32_t>((int32_t)k_nb); };           /* GetAvailableLanguageCodeCount */
        s->cmds[4] = [](Request &, Response &rs) { rs.push<uint32_t>(region_systeme()); };       /* GetRegionCode */
        s->cmds[5] = ecrire_codes;                                                               /* GetAvailableLanguageCodes2 */
        s->cmds[6] = [](Request &, Response &rs) { rs.push<int32_t>((int32_t)k_nb); };
        s->cmds[8] = [](Request &, Response &rs) { rs.push<uint8_t>(0); };                       /* GetQuestFlag */
        return s;
    });
    ipc_register("set:sys", [] {
        auto s = std::make_shared<TableService>("set:sys");
        auto fw = [](Request &rq, Response &) { /* GetFirmwareVersion(2) : FirmwareVersionFormat (0x100) */
            Buf b = rq.out();
            if (!b.addr) return;
            memset(g_mem + b.addr, 0, b.size);
            uint8_t v[0x100] = {};
            v[0] = 5; v[1] = 1; v[2] = 0; v[4] = 0; v[5] = 0; /* 5.1.0 */
            strcpy((char *)v + 0x08, "NX");
            strcpy((char *)v + 0x28, "5.1.0");
            strcpy((char *)v + 0x68, "NintendoSDK Firmware for NX 5.1.0-3.0");
            memcpy(g_mem + b.addr, v, b.size < sizeof v ? b.size : sizeof v);
        };
        s->cmds[3] = fw;
        s->cmds[4] = fw;
        s->cmds[23] = [](Request &, Response &rs) { rs.push<uint32_t>(0); }; /* GetColorSetId */
        return s;
    });
}
