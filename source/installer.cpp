#include "installer.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

#include "settings.hpp"
#include "util.hpp"

#ifdef HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

// Convierte la ruta de una entrada en una ruta relativa segura. false = descartar la entrada.
bool safeRelative(const char* pathname, std::string& out) {
    if (!pathname) return false;
    std::string p = pathname;
    for (auto& c : p)
        if (c == '\\') c = '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t e = p.find('/', i);
        if (e == std::string::npos) e = p.size();
        std::string comp = p.substr(i, e - i);
        i = e + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") return false;  // zip-slip
        comp = util::sanitizeComponent(comp);
        if (comp.empty()) continue;
        parts.push_back(comp);
    }
    if (parts.empty()) return false;
    out.clear();
    for (size_t k = 0; k < parts.size(); k++) out += (k ? "/" : "") + parts[k];
    return true;
}

// ------------------------------------------------------------------ extracción
#ifdef HAVE_LIBARCHIVE

bool extractArchive(const std::string& archivePath, const std::string& destDir, Progress* progress, std::string& err) {
    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, archivePath.c_str(), 1 << 16) != ARCHIVE_OK) {
        const char* m = archive_error_string(a);
        err = std::string(T("No se pudo abrir el archivo: ", "Could not open archive: ")) + (m ? m : "?");
        archive_read_free(a);
        return false;
    }
    util::mkdirs(destDir);
    if (progress) {
        progress->total = util::fileSize(archivePath);
        progress->now = 0;
    }
    std::vector<char> buf(1 << 16);
    struct archive_entry* e = nullptr;
    int r;
    bool ok = true;
    int files = 0;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (progress && progress->cancel.load()) {
            err = T("Cancelado", "Cancelled");
            ok = false;
            break;
        }
        std::string rel;
        if (!safeRelative(archive_entry_pathname(e), rel)) {
            archive_read_data_skip(a);
            continue;
        }
        std::string out = util::join(destDir, rel);
        unsigned type = archive_entry_filetype(e);
        if (type == AE_IFDIR) {
            util::mkdirs(out);
            continue;
        }
        if (type != AE_IFREG) {
            archive_read_data_skip(a);
            continue;
        }
        util::mkdirs(util::dirName(out));
        FILE* f = fopen(out.c_str(), "wb");
        if (!f) {
            err = std::string(T("No se pudo crear ", "Could not create ")) + out;
            ok = false;
            break;
        }
        ssize_t n;
        while ((n = archive_read_data(a, buf.data(), buf.size())) > 0) {
            if (fwrite(buf.data(), 1, (size_t)n, f) != (size_t)n) {
                err = T("Error de escritura (¿SD llena?)", "Write error (SD full?)");
                ok = false;
                break;
            }
        }
        if (fclose(f) != 0 && ok) {
            err = T("Error de escritura (¿SD llena?)", "Write error (SD full?)");
            ok = false;
        }
        if (n < 0 && ok) {
            const char* m = archive_error_string(a);
            err = std::string(T("Archivo dañado o formato no soportado: ", "Corrupt archive or unsupported format: ")) + (m ? m : "?");
            ok = false;
        }
        if (!ok) break;
        files++;
        if (progress) progress->now = (uint64_t)std::max<int64_t>(0, archive_filter_bytes(a, -1));
    }
    if (ok && r != ARCHIVE_EOF) {
        const char* m = archive_error_string(a);
        err = std::string(T("Error leyendo el archivo: ", "Error reading archive: ")) + (m ? m : "?");
        ok = false;
    }
    archive_read_free(a);
    if (ok && files == 0) {
        err = T("El archivo está vacío", "The archive is empty");
        ok = false;
    }
    if (progress && ok) progress->now = progress->total.load();
    return ok;
}

#else

bool extractArchive(const std::string&, const std::string&, Progress*, std::string& err) {
    err = "compilado sin libarchive";
    return false;
}

#endif

// ------------------------------------------------------------------ detección de raíces
static bool isKnownDir(const std::string& l) {
    static const char* known[] = {"fighter", "ui",   "sound", "stage",  "camera", "effect", "assist", "item", "param", "prebuilt",
                                  "append",  "movie", "stream", "spirits", "patch",  "render", "scene",  "vc",   "lua",   nullptr};
    for (int i = 0; known[i]; i++)
        if (l == known[i]) return true;
    return false;
}

static bool isIgnoredDir(const std::string& l) {
    return l == "__macosx" || l == "atmosphere" || l == "exefs" || l == "skyline" || l == "plugins" || l == "arcropolis" || l == ".git" || l == ".github";
}

static bool isModRoot(const std::string& dir) {
    for (const auto& d : util::listDirs(dir))
        if (isKnownDir(util::lower(d))) return true;
    for (const auto& f : util::listFiles(dir)) {
        std::string l = util::lower(f);
        if (l == "config.json" || l == "info.toml" || l == "preview.webp") return true;
    }
    return false;
}

static void findRoots(const std::string& dir, int depth, std::vector<std::string>& out) {
    if (depth > 6) return;
    if (isModRoot(dir)) {
        out.push_back(dir);
        return;
    }
    for (const auto& d : util::listDirs(dir)) {
        if (isIgnoredDir(util::lower(d))) continue;
        findRoots(util::join(dir, d), depth + 1, out);
    }
}

std::vector<std::string> findModRoots(const std::string& dir) {
    std::vector<std::string> out;
    findRoots(dir, 0, out);
    return out;
}

static bool isGenericName(const std::string& l) { return l == "mod" || l == "mods" || l == "files" || l == "file" || l == "data" || l == "ultimate" || l == "romfs" || l == "content"; }

static void cleanJunk(const std::string& root) {
    for (const auto& d : util::listDirs(root))
        if (util::lower(d) == "__macosx") util::rmrf(util::join(root, d));
    for (const auto& f : util::listFiles(root)) {
        std::string l = util::lower(f);
        if (l == ".ds_store" || l == "thumbs.db" || l == "desktop.ini") util::rmrf(util::join(root, f));
    }
}

InstallResult installFromDir(const std::string& dir, const std::string& fallbackName, const std::string& targetDir, const std::string& replaceFolder) {
    InstallResult res;
    std::vector<std::string> roots = findModRoots(dir);
    if (roots.empty()) {
        res.err = T("No se encontraron archivos de mod (fighter/, ui/, sound/...). Puede ser un plugin o un paquete que se instala a mano.",
                    "No mod files found (fighter/, ui/, sound/...). It may be a plugin or a package that must be installed manually.");
        return res;
    }
    if (!util::mkdirs(targetDir)) {
        res.err = std::string(T("No se pudo crear ", "Could not create ")) + targetDir;
        return res;
    }
    const bool replacing = roots.size() == 1 && !replaceFolder.empty();
    std::set<std::string> used;
    for (const auto& r : roots) {
        cleanJunk(r);
        std::string name = (r == dir) ? fallbackName : util::baseName(r);
        if (isGenericName(util::lower(name))) name = fallbackName;
        name = util::sanitizeName(name);
        if (replacing) name = replaceFolder;
        std::string finalName = name;
        std::string dst = util::join(targetDir, finalName);
        std::string backup;
        if (replacing) {
            // No se borra la versión vieja hasta saber que la nueva quedó bien colocada.
            if (util::exists(dst)) {
                backup = util::join(paths::tmp(), "replaced_" + util::fnvHex(dst));
                util::rmrf(backup);
                util::mkdirs(paths::tmp());
                if (!util::moveDir(dst, backup)) {
                    res.err = std::string(T("No se pudo retirar la versión anterior de ", "Could not remove the previous version of ")) + finalName;
                    return res;
                }
            }
        } else {
            int n = 2;
            while (util::exists(dst) || used.count(util::lower(finalName))) {
                finalName = name + " (" + std::to_string(n++) + ")";
                dst = util::join(targetDir, finalName);
            }
        }
        if (!util::moveDir(r, dst)) {
            if (!backup.empty()) {  // restaura la versión anterior
                util::rmrf(dst);
                util::moveDir(backup, dst);
            }
            res.err = std::string(T("No se pudo mover a ", "Could not move to ")) + dst;
            return res;
        }
        if (!backup.empty()) util::rmrf(backup);
        used.insert(util::lower(finalName));
        res.folders.push_back(finalName);
    }
    res.ok = true;
    return res;
}
