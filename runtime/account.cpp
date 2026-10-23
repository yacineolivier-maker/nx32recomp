/* Comptes : un seul utilisateur local. Interfaces : https://switchbrew.org/wiki/Account_services */
#include "ipc.h"

#include <cstdio>
#include <string>

/* identifiant de l'utilisateur (128 bits) et pseudonyme affiché en jeu */
const uint64_t g_uid[2] = {0x4D4B38504331ull, 0x4A4F55455552ull};
std::string config_texte(const char *section, const char *cle, const char *defaut); /* config.cpp */
/* [systeme] pseudo (lu à la première utilisation : config.cpp peut ne pas être encore initialisé au chargement) */
const char *pseudo() {
    static const std::string p = config_texte("systeme", "pseudo", "Joueur");
    return p.c_str();
}
extern const uint8_t g_mii_perso_id[16]; /* mii.cpp : Mii personnel du compte */

namespace {

void ok(Request &, Response &) {}

void push_uid(Response &rs) { rs.push<uint64_t>(g_uid[0]); rs.push<uint64_t>(g_uid[1]); }

/* ProfileBase (0x38) : uid, horodatage, pseudonyme (0x21) */
void ecrire_base(uint8_t *p) {
    memset(p, 0, 0x38);
    memcpy(p, g_uid, 16);
    uint64_t ts = 1700000000;
    memcpy(p + 0x10, &ts, 8);
    memcpy(p + 0x18, pseudo(), strlen(pseudo()) < 0x20 ? strlen(pseudo()) : 0x20); /* 0x21 avec le zéro final */
}

std::shared_ptr<Service> profile() {
    auto s = std::make_shared<TableService>("acc:IProfile");
    s->cmds[0] = [](Request &rq, Response &rs) { /* Get -> ProfileBase + UserData (B 0x80) */
        uint8_t base[0x38];
        ecrire_base(base);
        Buf b = rq.out();
        if (b.addr) { /* UserData : icône du compte = Mii personnel (miiId à +0x10, cf. mii.cpp) */
            memset(g_mem + b.addr, 0, b.size);
            if (b.size >= 0x20) memcpy(g_mem + b.addr + 0x10, g_mii_perso_id, 16);
        }
        rs.data.insert(rs.data.end(), base, base + 0x38);
    };
    s->cmds[1] = [](Request &, Response &rs) { /* GetBase */
        uint8_t base[0x38];
        ecrire_base(base);
        rs.data.insert(rs.data.end(), base, base + 0x38);
    };
    s->cmds[10] = [](Request &, Response &rs) { rs.push<uint32_t>(0); }; /* GetImageSize : pas d'image */
    s->cmds[11] = [](Request &, Response &rs) { rs.push<uint32_t>(0); }; /* LoadImage */
    return s;
}

std::shared_ptr<Service> manager_for_application() {
    auto s = std::make_shared<TableService>("acc:IManagerForApplication");
    s->cmds[0] = ok;                                                    /* CheckAvailability */
    s->cmds[1] = [](Request &, Response &rs) { rs.push<uint64_t>(g_uid[0]); }; /* GetAccountId */
    return s;
}

std::shared_ptr<Service> acc() {
    auto s = std::make_shared<TableService>("acc:u0");
    s->cmds[0] = [](Request &, Response &rs) { rs.push<uint32_t>(1); };   /* GetUserCount */
    s->cmds[1] = [](Request &, Response &rs) { rs.push<uint8_t>(1); };    /* GetUserExistence */
    auto liste = [](Request &rq, Response &) {                            /* ListAllUsers / ListOpenUsers */
        Buf b = rq.out();
        if (b.addr && b.size >= 16) { memset(g_mem + b.addr, 0, b.size); memcpy(g_mem + b.addr, g_uid, 16); }
    };
    s->cmds[2] = liste;
    s->cmds[3] = liste;
    s->cmds[4] = [](Request &, Response &rs) { push_uid(rs); };           /* GetLastOpenedUser */
    s->cmds[5] = [](Request &, Response &rs) { rs.objects.push_back(profile()); }; /* GetProfile */
    s->cmds[50] = [](Request &, Response &rs) { rs.push<uint8_t>(0); };   /* IsUserRegistrationRequestPermitted */
    s->cmds[51] = [](Request &, Response &rs) { push_uid(rs); };          /* TrySelectUserWithoutInteraction */
    s->cmds[100] = ok;                                                    /* InitializeApplicationInfo */
    s->cmds[101] = [](Request &, Response &rs) { rs.objects.push_back(manager_for_application()); };
    s->cmds[110] = ok; s->cmds[111] = ok;                                 /* miniatures de sauvegarde */
    s->cmds[130] = ok;                                                    /* LoadOpenContext */
    s->cmds[140] = liste;                                                 /* ListQualifiedUsers */
    return s;
}

} // namespace

/* IStorage (paramètres de lancement de l'applet) : Open -> IStorageAccessor (GetSize, Read) */
std::shared_ptr<Service> applet_storage(std::vector<uint8_t> data) {
    auto st = std::make_shared<TableService>("am:IStorage");
    auto d = std::make_shared<std::vector<uint8_t>>(std::move(data));
    st->cmds[0] = [d](Request &, Response &rs) {
        auto acc = std::make_shared<TableService>("am:IStorageAccessor");
        acc->cmds[0] = [d](Request &, Response &r2) { r2.push<int64_t>((int64_t)d->size()); };
        acc->cmds[11] = [d](Request &rq, Response &) {
            int64_t off = rq.pop<int64_t>();
            Buf b = rq.out();
            if (off >= 0 && (size_t)off < d->size() && b.addr)
                memcpy(g_mem + b.addr, d->data() + off, std::min<size_t>(b.size, d->size() - (size_t)off));
        };
        rs.objects.push_back(acc);
    };
    return st;
}

/* Paramètre de lancement « utilisateur présélectionné » (kind 2) */
std::vector<uint8_t> launch_param_preselected_user() {
    std::vector<uint8_t> v(0x88, 0);
    uint32_t magic = 0xC79497CAu, selected = 1;
    memcpy(v.data(), &magic, 4);
    memcpy(v.data() + 4, &selected, 4);
    memcpy(v.data() + 8, g_uid, 16);
    return v;
}

void account_register() {
    for (auto n : {"acc:u0", "acc:u1"}) ipc_register(n, acc);
}
