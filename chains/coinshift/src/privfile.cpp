// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <coinshift/privfile.h>

#include <bt/fsperm.h>

#include <filesystem>
#include <random>

#ifdef _WIN32
#include <fstream>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace coinshift {

bool write_private_file(const std::string& path, const std::string& content, std::string& err)
{
    std::filesystem::path p(path);
    if (p.has_parent_path() && !bt::fsperm::private_dir(p.parent_path().string(), false)) {
        err = "cannot create " + p.parent_path().string();
        return false;
    }
    std::error_code ec;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(p, ec))) {
        err = path + " is a symbolic link";
        return false;
    }
    std::string tmp = path + ".tmp." + std::to_string(std::random_device{}());
#ifdef _WIN32
    if (!bt::fsperm::private_file(tmp, false)) { // created empty with an owner-only DACL
        err = "cannot create " + tmp;
        return false;
    }
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << content;
        f.flush();
        if (!f) {
            err = "cannot write " + tmp;
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
#else
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        err = "cannot create " + tmp + ": " + std::strerror(errno);
        return false;
    }
    size_t off = 0;
    while (off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = std::string("cannot write: ") + std::strerror(errno);
            ::close(fd);
            ::unlink(tmp.c_str());
            return false;
        }
        off += size_t(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        err = std::string("cannot sync: ") + std::strerror(errno);
        ::unlink(tmp.c_str());
        return false;
    }
#endif
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        err = "cannot rename: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace coinshift
