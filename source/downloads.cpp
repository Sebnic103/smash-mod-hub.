#include "downloads.hpp"

#include <cstdio>
#include <ctime>

#include "http.hpp"
#include "installer.hpp"
#include "md5.hpp"
#include "settings.hpp"
#include "util.hpp"

void Downloads::start() {
    quit_ = false;
    th_.start([this] { run(); }, 0x100000);
}

void Downloads::stop() {
    quit_ = true;
    {
        sys::Lock l(mx_);
        for (auto& j : jobs_) j->prog.cancel = true;
    }
    th_.join();
}

std::shared_ptr<Job> Downloads::enqueue(const gb::Item& item, const gb::File& file, const std::string& replaceFolder, const std::string& targetDir) {
    auto j = std::make_shared<Job>();
    j->item = item;
    j->file = file;
    j->replaceFolder = replaceFolder;
    j->targetDir = targetDir;
    sys::Lock l(mx_);
    j->id = nextId_++;
    jobs_.push_back(j);
    return j;
}

std::shared_ptr<Job> Downloads::enqueueLocal(const std::string& archivePath, const std::string& displayName, const std::string& targetDir) {
    auto j = std::make_shared<Job>();
    j->item.name = displayName;
    j->item.model = "Mod";
    j->file.name = util::baseName(archivePath);
    j->file.size = util::fileSize(archivePath);
    j->targetDir = targetDir;
    j->localPath = archivePath;
    sys::Lock l(mx_);
    j->id = nextId_++;
    jobs_.push_back(j);
    return j;
}

std::vector<std::shared_ptr<Job>> Downloads::jobs() {
    sys::Lock l(mx_);
    return jobs_;
}

std::vector<std::shared_ptr<Job>> Downloads::takeFinished() {
    std::vector<std::shared_ptr<Job>> out;
    sys::Lock l(mx_);
    for (auto& j : jobs_) {
        int s = j->state.load();
        if ((s == Job::Done || s == Job::Failed) && !j->reported) {
            j->reported = true;
            out.push_back(j);
        }
    }
    return out;
}

void Downloads::cancel(int id) {
    sys::Lock l(mx_);
    for (auto& j : jobs_) {
        if (j->id != id) continue;
        int expected = Job::Queued;
        if (j->state.compare_exchange_strong(expected, Job::Cancelled)) return;
        j->prog.cancel = true;
    }
}

void Downloads::remove(int id) {
    sys::Lock l(mx_);
    for (size_t i = 0; i < jobs_.size(); i++) {
        if (jobs_[i]->id != id) continue;
        int s = jobs_[i]->state.load();
        if (s == Job::Done || s == Job::Failed || s == Job::Cancelled) jobs_.erase(jobs_.begin() + (long)i);
        return;
    }
}

void Downloads::clearFinished() {
    sys::Lock l(mx_);
    for (size_t i = 0; i < jobs_.size();) {
        int s = jobs_[i]->state.load();
        if (s == Job::Done || s == Job::Failed || s == Job::Cancelled) jobs_.erase(jobs_.begin() + (long)i);
        else i++;
    }
}

int Downloads::activeCount() {
    int n = 0;
    sys::Lock l(mx_);
    for (auto& j : jobs_) {
        int s = j->state.load();
        if (s == Job::Queued || (s >= Job::Downloading && s <= Job::Installing)) n++;
    }
    return n;
}

void Downloads::run() {
    uint64_t lastTrim = sys::nowMs();
    while (!quit_.load()) {
        std::shared_ptr<Job> job;
        {
            sys::Lock l(mx_);
            for (auto& j : jobs_) {
                int expected = Job::Queued;
                if (j->state.compare_exchange_strong(expected, Job::Downloading)) {
                    job = j;
                    break;
                }
            }
        }
        if (!job) {
            sys::sleepMs(100);
            if (sys::nowMs() - lastTrim > 2000) {
                lastTrim = sys::nowMs();
                http::trimIdle();
                util::flushLog();  // el registro se guarda en bloques; aquí, sin molestar a la interfaz
            }
            continue;
        }
        process(*job);
    }
}

void Downloads::process(Job& j) {
    auto fail = [&](const std::string& e) {
        j.error = e;
        j.state = Job::Failed;
        util::log("job %d fallo: %s", j.id, e.c_str());
    };
    auto cancelled = [&]() {
        j.state = Job::Cancelled;
        util::log("job %d cancelado", j.id);
    };

    uint64_t freeB = util::freeSpace(paths::root() + "/");
    if (freeB && j.file.size && j.file.size * 3 > freeB) {
        fail(T("Espacio insuficiente en la SD", "Not enough free space on the SD card"));
        return;
    }

    const bool local = !j.localPath.empty();
    std::string archive = local ? j.localPath : util::join(paths::downloads(), std::to_string(j.file.id) + "_" + util::sanitizeComponent(j.file.name));
    std::string tmp = util::join(paths::tmp(), "job" + std::to_string(j.id));
    // Los archivos importados son del usuario: solo se mueven a import/done al terminar bien.
    const bool deleteArchive = !local && !g_settings.keepArchives;

    // 1) Descarga
    if (!local) {
        j.state = Job::Downloading;
        j.prog.now = 0;
        j.prog.total = j.file.size;
        http::Result r = http::downloadBig(j.file.url, archive, &j.prog, g_settings.dlConns);
        if (j.prog.cancel.load()) return cancelled();
        if (!r.ok()) return fail(std::string(T("Descarga fallida: ", "Download failed: ")) + r.describe());
    }

    // 2) MD5 (si GameBanana lo informa)
    if (!local && g_settings.verifyMd5 && !j.file.md5.empty()) {
        j.state = Job::Verifying;
        j.prog.now = 0;
        j.prog.total = util::fileSize(archive);
        std::string h = md5::file(archive, &j.prog);
        if (j.prog.cancel.load()) {
            util::rmrf(archive);
            return cancelled();
        }
        if (h != j.file.md5) {
            util::rmrf(archive);
            return fail(T("El MD5 no coincide (descarga corrupta). Inténtalo de nuevo.", "MD5 mismatch (corrupt download). Try again."));
        }
    }

    // 3) Extracción
    j.state = Job::Extracting;
    j.prog.now = 0;
    j.prog.total = util::fileSize(archive);
    util::rmrf(tmp);
    util::mkdirs(tmp);
    std::string err;
    if (!extractArchive(archive, tmp, &j.prog, err)) {
        util::rmrf(tmp);
        if (deleteArchive) util::rmrf(archive);
        if (j.prog.cancel.load()) return cancelled();
        return fail(err);
    }

    // 4) Instalación en ultimate/mods
    j.state = Job::Installing;
    InstallResult ir = installFromDir(tmp, j.item.name, j.targetDir, j.replaceFolder);
    util::rmrf(tmp);
    if (deleteArchive) util::rmrf(archive);
    if (!ir.ok) return fail(ir.err);
    if (local) {
        std::string doneDir = util::join(util::dirName(archive), "done");
        util::mkdirs(doneDir);
        std::string dst = util::join(doneDir, util::baseName(archive));
        util::rmrf(dst);
        if (rename(archive.c_str(), dst.c_str()) != 0) util::log("no se pudo mover %s a import/done", archive.c_str());
    }

    // 5) Miniatura para la lista de instalados (mejor esfuerzo)
    std::string thumbPath;
    std::string turl = j.item.storeThumbUrl();
    if (!local && !turl.empty()) {
        thumbPath = util::join(paths::thumbs(), j.item.model + "_" + std::to_string(j.item.id) + ".img");
        Progress p;
        http::Result tr = http::download(turl, thumbPath, &p);
        if (!tr.ok()) thumbPath.clear();
    }

    j.meta = Meta();
    j.meta.gbId = j.item.id;
    j.meta.model = j.item.model;
    j.meta.name = j.item.name;
    j.meta.author = j.item.author;
    j.meta.version = j.item.version;
    j.meta.category = j.item.category();
    j.meta.thumb = thumbPath;
    j.meta.fileId = j.file.id;
    j.meta.latestFileId = j.file.id;
    j.meta.installedAt = util::formatDate((int64_t)time(nullptr));
    j.folders = ir.folders;
    j.state = Job::Done;
    util::log("job %d instalado: %s", j.id, j.item.name.c_str());
}
