/* fsp-srv : RomFS (stockage brut lu depuis romfs.bin) et données de sauvegarde (dossier hôte).
 * Interfaces : https://switchbrew.org/wiki/Filesystem_services */
#include "ipc.h"
#include "jeu.h"
#include "mods.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <io.h>
#include <filesystem>
#include <mutex>

namespace fs = std::filesystem;
extern "C" void rt_ecriture_hote(uint32_t adresse, uint32_t taille); /* eden_lien.cpp */
extern "C" int config_oui(const char *section, const char *cle, int defaut); /* config.cpp */

/* disposition de développement : données du jeu dans NX_DONNEES (romfs.bin, archives système systeme/, polices/) */
std::string g_donnees_dir = getenv("NX_DONNEES") ? getenv("NX_DONNEES") : "donnees";
std::string g_romfs_path = g_donnees_dir + "/romfs.bin";
std::string g_save_dir = "build/sauvegarde";
std::string g_mods_dir = "build/mods";       /* mods/<nom>/romfs/ : fichiers remplaçant ceux du jeu (mods.cpp) */

/* Installation (Jeu/) : si donnees/image.bin est à côté de l'exécutable, tous les chemins sont relatifs à son dossier
 * (le dossier courant devient celui de l'exécutable : raccourci, double-clic). Sinon : disposition de développement. */
extern "C" const char *chemins_init(const char *image_arg) {
    static std::string image;
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    while (n && exe[n - 1] != '\\') --n;
    exe[n] = 0;
    const std::string dossier = exe;
    if (!image_arg && fs::exists(dossier + "donnees\\image.bin")) {
        SetCurrentDirectoryA(dossier.c_str());
        g_donnees_dir = "donnees";
        g_romfs_path = "donnees/romfs.bin";
        g_save_dir = "sauvegarde";
        g_mods_dir = "mods";
        image = "donnees/image.bin";
        fprintf(stderr, "[rt] installation : %s\n", dossier.c_str());
        return image.c_str();
    }
    return image_arg ? image_arg : "image.bin";
}

namespace {

constexpr uint32_t RES_PATH_NOT_FOUND = 0x202;
constexpr uint32_t RES_PATH_EXISTS = 0x402;

void copy_out(Buf b, const void *src, size_t n) { memcpy(g_mem + b.addr, src, n < b.size ? n : b.size); }

/* ------------------------------------------------------------------ IStorage sur un fichier hôte */
struct HostStorage : Service {
    FILE *f;
    uint64_t size = 0;
    std::mutex m;
    HostStorage(const std::string &path) : Service("fs:IStorage") {
        f = fopen(path.c_str(), "rb");
        if (!f) fprintf(stderr, "[fs] impossible d'ouvrir %s\n", path.c_str());
        else { _fseeki64(f, 0, SEEK_END); size = (uint64_t)_ftelli64(f); }
    }
    ~HostStorage() override { if (f) fclose(f); }
    bool handle(Request &rq, Response &rs) override {
        switch (rq.cmd) {
        case 0: { /* Read(offset, size, B) */
            uint64_t off = rq.pop<uint64_t>(), n = rq.pop<uint64_t>();
            Buf b = rq.out();
            if (n > b.size) n = b.size;
            std::lock_guard<std::mutex> lk(m);
            static const bool trace = getenv("NX_FS_TRACE") != nullptr;
            if (trace) fprintf(stderr, "[fs] lecture %llX %llX%c", (unsigned long long)off, (unsigned long long)n, 10);
            _fseeki64(f, (long long)off, SEEK_SET);
            rt_ecriture_hote(b.addr, (uint32_t)n);
            fread(g_mem + b.addr, 1, (size_t)n, f);
            return true;
        }
        case 4: rs.push<uint64_t>(size); return true; /* GetSize */
        }
        return false;
    }
};

/* ------------------------------------------------------------------ IStorage du RomFS avec mods (mods.cpp) */
struct ModStorage : Service {
    std::shared_ptr<ImageRomfs> img;
    ModStorage(std::shared_ptr<ImageRomfs> i) : Service("fs:IStorage"), img(std::move(i)) {}
    bool handle(Request &rq, Response &rs) override {
        switch (rq.cmd) {
        case 0: { /* Read(offset, size, B) */
            uint64_t off = rq.pop<uint64_t>(), n = rq.pop<uint64_t>();
            Buf b = rq.out();
            if (n > b.size) n = b.size;
            static const bool trace = getenv("NX_FS_TRACE") != nullptr;
            if (trace) fprintf(stderr, "[fs] lecture mods %llX %llX%c", (unsigned long long)off, (unsigned long long)n, 10);
            rt_ecriture_hote(b.addr, (uint32_t)n);
            img->lire(off, n, g_mem + b.addr);
            return true;
        }
        case 4: rs.push<uint64_t>(img->taille); return true; /* GetSize */
        }
        return false;
    }
};

/* RomFS du jeu : construit une fois (mods désactivables par [mods] actifs = non ; fichiers générés par le jeu) */
std::shared_ptr<Service> romfs_jeu() {
    static std::once_flag once;
    static std::shared_ptr<ImageRomfs> img;
    std::call_once(once, [] {
        auto i = std::make_shared<ImageRomfs>();
        jeu_generateurs_romfs(*i); /* fichiers réécrits à la volée (réglages propres au jeu) */
        if (i->ouvrir(g_romfs_path, config_oui("mods", "actifs", 1) ? g_mods_dir : std::string())) img = i;
    });
    if (img) return std::make_shared<ModStorage>(img);
    return std::make_shared<HostStorage>(g_romfs_path);
}

/* ------------------------------------------------------------------ IFileSystem sur un dossier hôte */
fs::path host_path(const fs::path &root, const std::string &p) {
    std::string rel = p;
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    return root / fs::u8path(rel);
}

struct HostFile : Service {
    FILE *f;
    HostFile(FILE *fp) : Service("fs:IFile"), f(fp) {}
    ~HostFile() override { if (f) fclose(f); }
    bool handle(Request &rq, Response &rs) override {
        switch (rq.cmd) {
        case 0: { /* Read(option, offset, size, B) -> u64 */
            rq.pop<uint32_t>();
            uint64_t off = rq.pop<uint64_t>(), n = rq.pop<uint64_t>();
            Buf b = rq.out();
            if (n > b.size) n = b.size;
            _fseeki64(f, (long long)off, SEEK_SET);
            rt_ecriture_hote(b.addr, (uint32_t)n);
            size_t got = fread(g_mem + b.addr, 1, (size_t)n, f);
            rs.push<uint64_t>(got);
            return true;
        }
        case 1: { /* Write(option, offset, size, A) */
            rq.pop<uint32_t>();
            uint64_t off = rq.pop<uint64_t>(), n = rq.pop<uint64_t>();
            Buf b = rq.in();
            _fseeki64(f, (long long)off, SEEK_SET);
            fwrite(g_mem + b.addr, 1, (size_t)(n < b.size ? n : b.size), f);
            return true;
        }
        case 2: fflush(f); return true;
        case 3: { /* SetSize */
            uint64_t n = rq.pop<uint64_t>();
            fflush(f);
            _chsize_s(_fileno(f), (long long)n);
            return true;
        }
        case 4: { _fseeki64(f, 0, SEEK_END); rs.push<uint64_t>((uint64_t)_ftelli64(f)); return true; }
        }
        return false;
    }
};

struct HostDir : Service {
    std::vector<fs::directory_entry> entries;
    size_t pos = 0;
    HostDir(const fs::path &p, uint32_t filter) : Service("fs:IDirectory") {
        std::error_code ec;
        for (auto &e : fs::directory_iterator(p, ec)) {
            bool d = e.is_directory();
            if ((d && (filter & 1)) || (!d && (filter & 2))) entries.push_back(e);
        }
    }
    bool handle(Request &rq, Response &rs) override {
        switch (rq.cmd) {
        case 0: { /* Read(B entrées 0x310) -> u64 */
            Buf b = rq.out();
            uint64_t n = 0;
            while (pos < entries.size() && (n + 1) * 0x310 <= b.size) {
                uint8_t ent[0x310] = {};
                auto u8 = entries[pos].path().filename().u8string();
                std::string name(u8.begin(), u8.end());
                memcpy(ent, name.c_str(), name.size() < 0x300 ? name.size() : 0x300);
                bool d = entries[pos].is_directory();
                ent[0x304] = d ? 0 : 1;
                uint64_t sz = d ? 0 : (uint64_t)entries[pos].file_size();
                memcpy(ent + 0x308, &sz, 8);
                memcpy(g_mem + b.addr + n * 0x310, ent, 0x310);
                n++, pos++;
            }
            rs.push<uint64_t>(n);
            return true;
        }
        case 1: rs.push<uint64_t>(entries.size()); return true;
        }
        return false;
    }
};

struct HostFs : Service {
    fs::path root;
    HostFs(fs::path r, const char *n) : Service(n), root(std::move(r)) { fs::create_directories(root); }
    bool handle(Request &rq, Response &rs) override {
        std::error_code ec;
        auto path = [&](int i = 0) { return host_path(root, rq.str(rq.x.size() > (size_t)i ? rq.x[i] : Buf{})); };
        static const bool trace = getenv("NX_FS_TRACE") != nullptr;
        if (trace) fprintf(stderr, "[fs] %s cmd %u : %s\n", name.c_str(), rq.cmd, rq.str(rq.x.empty() ? Buf{} : rq.x[0]).c_str());
        switch (rq.cmd) {
        case 0: { /* CreateFile(option, size, path) */
            rq.pop<uint32_t>();
            uint64_t size = rq.pop<uint64_t>();
            auto p = path();
            if (fs::exists(p)) { rs.result = RES_PATH_EXISTS; return true; }
            FILE *f = _wfopen(p.c_str(), L"wb");
            if (!f) { rs.result = RES_PATH_NOT_FOUND; return true; }
            _chsize_s(_fileno(f), (long long)size);
            fclose(f);
            return true;
        }
        case 1: if (!fs::remove(path(), ec)) rs.result = RES_PATH_NOT_FOUND; return true;
        case 2: if (!fs::create_directory(path(), ec)) rs.result = fs::exists(path()) ? RES_PATH_EXISTS : RES_PATH_NOT_FOUND; return true;
        case 3: if (!fs::remove(path(), ec)) rs.result = RES_PATH_NOT_FOUND; return true;
        case 4: fs::remove_all(path(), ec); return true;
        case 5: case 6: fs::rename(path(0), path(1), ec); if (ec) rs.result = RES_PATH_NOT_FOUND; return true;
        case 7: { /* GetEntryType -> u32 (0 dossier, 1 fichier) */
            auto p = path();
            if (!fs::exists(p)) { rs.result = RES_PATH_NOT_FOUND; return true; }
            rs.push<uint32_t>(fs::is_directory(p) ? 0 : 1);
            return true;
        }
        case 8: { /* OpenFile(mode, path) */
            uint32_t mode = rq.pop<uint32_t>();
            auto p = path();
            if (!fs::is_regular_file(p)) { rs.result = RES_PATH_NOT_FOUND; return true; }
            FILE *f = _wfopen(p.c_str(), (mode & 2) ? L"r+b" : L"rb");
            if (!f) { rs.result = RES_PATH_NOT_FOUND; return true; }
            rs.objects.push_back(std::make_shared<HostFile>(f));
            return true;
        }
        case 9: { /* OpenDirectory(filter, path) */
            uint32_t filter = rq.pop<uint32_t>();
            auto p = path();
            if (!fs::is_directory(p)) { rs.result = RES_PATH_NOT_FOUND; return true; }
            rs.objects.push_back(std::make_shared<HostDir>(p, filter));
            return true;
        }
        case 10: return true; /* Commit */
        case 11: rs.push<uint64_t>(512ull << 20); return true;
        case 12: rs.push<uint64_t>(1ull << 30); return true;
        case 13: { /* CleanDirectoryRecursively */
            for (auto &e : fs::directory_iterator(path(), ec)) fs::remove_all(e.path(), ec);
            return true;
        }
        case 14: { for (int k = 0; k < 4; k++) rs.push<uint64_t>(0); return true; }
        }
        return false;
    }
};

} // namespace

void fs_register() {
    ipc_register("fsp-srv", [] {
        auto s = std::make_shared<TableService>("fsp-srv");
        s->cmds[1] = [](Request &, Response &) {};                                  /* SetCurrentProcess */
        s->cmds[18] = [](Request &, Response &rs) {                                  /* OpenSdCardFileSystem */
            rs.objects.push_back(std::make_shared<HostFs>(fs::path(g_save_dir) / "sdcard", "fs:IFileSystem(sd)"));
        };
        auto save = [](Request &rq, Response &rs) {                                  /* OpenSaveDataFileSystem */
            if (getenv("NX_FS_TRACE")) {
                fprintf(stderr, "[fs] OpenSaveDataFileSystem cmd %u :", rq.cmd);
                for (size_t k = 0; k < rq.data.size() && k < 0x50; k++) fprintf(stderr, " %02X", rq.data[k]);
                fprintf(stderr, "\n");
            }
            /* un dossier par type de sauvegarde (SaveDataAttribute.type à +0x28) : le jeu ouvre d'abord la sauvegarde
             * « appareil » (3) et la vide ; partagée avec celle du compte (1), elle effaçait userdata.dat à chaque
             * démarrage (sauvegarde neuve, choix du Mii redemandé) */
            const uint8_t type = rq.data.size() > 0x28 ? rq.data[0x28] : 1;
            const char *dossier = type == 1 ? "save" : type == 3 ? "appareil" : type == 5 ? "cache" : "autre";
            rs.objects.push_back(std::make_shared<HostFs>(fs::path(g_save_dir) / dossier, "fs:IFileSystem(save)"));
        };
        s->cmds[51] = save;
        s->cmds[52] = save;
        s->cmds[22] = [](Request &, Response &) {};                                  /* CreateSaveDataFileSystem */
        s->cmds[200] = [](Request &, Response &rs) {                                 /* OpenDataStorageByCurrentProcess */
            rs.objects.push_back(romfs_jeu());
        };
        s->cmds[203] = [](Request &, Response &rs) { rs.result = 0x7D202; };        /* OpenPatchDataStorageByCurrentProcess : pas de mise à jour */
        s->cmds[202] = [](Request &rq, Response &rs) {                               /* OpenDataStorageByDataId(storage, dataId) */
            uint8_t storage = rq.pop<uint8_t>();
            uint64_t id = rq.pop<uint64_t>();
            char chemin[128];
            snprintf(chemin, sizeof chemin, "%s/systeme/%016llX.romfs", g_donnees_dir.c_str(), (unsigned long long)id);
            FILE *t = fopen(chemin, "rb");
            fprintf(stderr, "[fs] OpenDataStorageByDataId(stockage %u, %016llX) -> %s\n", storage, (unsigned long long)id,
                    t ? "trouvé" : "ABSENT");
            if (!t) { rs.result = 0x7D202; return; }
            fclose(t);
            rs.objects.push_back(std::make_shared<HostStorage>(chemin));
        };
        s->cmds[1003] = [](Request &, Response &) {};
        s->cmds[1004] = [](Request &, Response &) {};
        s->cmds[1005] = [](Request &, Response &rs) { rs.push<uint32_t>(0); };   /* GetGlobalAccessLogMode */
        s->cmds[1006] = [](Request &, Response &) {};
        s->cmds[1011] = [](Request &, Response &rs) { rs.push<uint32_t>(0); };
        return s;
    });
}
