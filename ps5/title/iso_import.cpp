#include "iso_import.h"
#include "../../darkness-ps5/runtime/native/xex_crypto_portable.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Oogie {
namespace {
namespace fs = std::filesystem;
constexpr uint64_t sector = 2048, maxBytes = 16ull << 30;
constexpr size_t maxEntries = 200000;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
uint32_t le32(const uint8_t* b) {
    return uint32_t(b[0]) | uint32_t(b[1])<<8 | uint32_t(b[2])<<16 | uint32_t(b[3])<<24;
}
uint16_t le16(const uint8_t* b) { return uint16_t(b[0]) | uint16_t(b[1])<<8; }
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    return s;
}
bool pathExists(const fs::path& p) {
    struct stat info{};
    if (stat(p.c_str(), &info) == 0) return true;
    if (errno == ENOENT) return false;
    throw std::system_error(errno, std::generic_category(), "Cannot check import path");
}
class Image {
    int fd = -1;
public:
    uint64_t size = 0;
    explicit Image(const fs::path& path) {
        fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0)
            throw std::system_error(errno, std::generic_category(), "Cannot open ISO");
        struct stat info{};
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
            close(fd);
            throw std::runtime_error("ISO must be a readable regular file");
        }
        size = static_cast<uint64_t>(info.st_size);
    }
    ~Image() { close(fd); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    void read(uint64_t offset, std::span<uint8_t> data) {
        require(offset <= size && data.size() <= size-offset, "ISO contains an out-of-bounds extent");
        while (!data.empty()) {
            const auto n = pread(fd, data.data(), data.size(), static_cast<off_t>(offset));
            if (n < 0 && errno == EINTR) continue;
            require(n > 0, "ISO read failed or was truncated");
            offset += n;
            data = data.subspan(static_cast<size_t>(n));
        }
    }
};
void safeName(const std::string& s) {
    require(!s.empty() && s != "." && s != "..", "ISO contains an unsafe filename");
    for (unsigned char c : s)
        require(c >= 32 && c != 127 && c != '/' && c != '\\' && c != ':', "ISO contains an unsafe filename");
}
void directory(const fs::path& p) {
    if (mkdir(p.c_str(), 0755) != 0 && errno != EEXIST)
        throw std::system_error(errno, std::generic_category(), "Cannot create import directory");
    struct stat info{};
    require(stat(p.c_str(), &info) == 0 && S_ISDIR(info.st_mode), "Import path is not a real directory");
}
}

IsoPlan inspectGameIso(const fs::path& path, const VerifyExecutable& verify) {
    require(bool(verify), "Executable verification is required");
    Image image(path);
    uint64_t base = 0;
    std::array<uint8_t,2048> header{};
    bool found = false;
    // XDVDFS partition offsets used for XISO, XGD2, XGD3 and XGD1 images.
    // Format reference: XboxDev/extract-xiso (offsets, media descriptor, entry layout).
    for (uint64_t candidate : {0ull, 0x0fd90000ull, 0x02080000ull, 0x18300000ull}) {
        if (candidate + 0x10000 + header.size() > image.size) continue;
        image.read(candidate + 0x10000, header);
        const std::string magic = "MICROSOFT*XBOX*MEDIA";
        if (std::equal(magic.begin(),magic.end(),header.begin()) &&
            std::equal(magic.begin(),magic.end(),header.end()-magic.size())) {
            base = candidate; found = true; break;
        }
    }
    require(found, "Not a supported Xbox disc ISO (XDVDFS partition not found)");
    IsoPlan plan; plan.source = path; plan.sourceSize = image.size;
    struct Pending { fs::path path; uint32_t sector, size; unsigned depth; };
    std::vector<Pending> dirs{{{},le32(header.data()+20),le32(header.data()+24),0}};
    std::set<std::pair<uint32_t,uint32_t>> tables;
    std::set<std::string> paths;
    while (!dirs.empty()) {
        auto dir = dirs.back(); dirs.pop_back();
        require(dir.depth <= 64, "ISO directories are too deeply nested");
        if (!dir.size) continue;
        require(dir.size <= 16*1024*1024 && tables.emplace(dir.sector,dir.size).second,
                "Invalid, repeated, or oversized ISO directory");
        std::vector<uint8_t> table(dir.size);
        image.read(base + uint64_t(dir.sector)*sector, table);
        std::vector<uint32_t> nodes{0};
        std::map<uint32_t,uint32_t> occupied;
        while (!nodes.empty()) {
            uint32_t at = nodes.back(); nodes.pop_back();
            require(at <= table.size() && table.size()-at >= 14, "Truncated ISO directory entry");
            const auto* b = table.data()+at;
            const uint32_t end = at+14+b[13];
            require(end <= table.size(), "Truncated ISO filename");
            auto next = occupied.lower_bound(at);
            require((next == occupied.end() || next->first >= end) &&
                    (next == occupied.begin() || std::prev(next)->second <= at),
                    "Cyclic or overlapping ISO directory entries");
            occupied.emplace(at,end);
            for (uint32_t child : {uint32_t(le16(b))*4, uint32_t(le16(b+2))*4})
                if (child) nodes.push_back(child);
            std::string name(reinterpret_cast<const char*>(b+14),b[13]);
            safeName(name);
            if (dir.path.empty())
                require(lower(name) != "oogie_paths.txt" && lower(name) != ".oogie-import-owner",
                        "ISO contains a reserved filename");
            if (dir.path.empty() && lower(name) == "$systemupdate") continue;
            if (dir.path.empty() && lower(name) == "default.xbe") name = "default.xbe";
            auto relative = dir.path/name;
            require(relative.generic_string().size() < 240 && paths.insert(lower(relative.generic_string())).second,
                    "Duplicate or excessively long ISO path");
            const uint32_t extent = le32(b+4), size = le32(b+8);
            const bool isDir = (b[12]&0x10) != 0;
            const uint64_t offset = base+uint64_t(extent)*sector;
            require(offset <= image.size && size <= image.size-offset, "ISO contains an out-of-bounds file");
            require(plan.entries.size() < maxEntries, "ISO contains too many files");
            plan.entries.push_back({relative,offset,size,isDir});
            if (isDir) dirs.push_back({relative,extent,size,dir.depth+1});
            else {
                require(size <= maxBytes-plan.bytes, "ISO extraction is too large");
                plan.bytes += size;
            }
        }
    }
    const auto xbe = std::find_if(plan.entries.begin(),plan.entries.end(),[](const IsoEntry& e) {
        return e.path == "default.xbe" && !e.directory;
    });
    require(xbe != plan.entries.end(), "ISO has no root default.xbe");
    require(xbe->size >= 24 && xbe->size <= 64*1024*1024, "Invalid game executable size");
    std::vector<uint8_t> executable(xbe->size);
    image.read(xbe->offset, executable);
    verify(executable);
    plan.executableHash = DarkRecomp::Native::XexCrypto::Sha256Hex(executable);
    return plan;
}

void extractGameIso(const IsoPlan& plan, const fs::path& destination, const ImportProgress& progress) {
    require(!destination.filename().empty(), "Invalid import destination");
    const auto parent = destination.parent_path();
    require(fs::is_directory(parent), "Import destination parent does not exist");
    require(!pathExists(destination), "Game folder already exists; it will not be overwritten");
    Image image(plan.source);
    require(image.size == plan.sourceSize, "ISO changed after inspection");
    std::error_code ec;
    const auto space = fs::space(parent,ec);
    if (!ec) require(space.available >= plan.bytes + (64ull<<20), "Not enough free space to extract the ISO");
    // Reserve a new private staging directory. The console can create and
    // stat known paths even when the app is not allowed to list directories.
    auto stagingTemplate = destination.string() + ".importing.XXXXXX";
    if (!mkdtemp(stagingTemplate.data()))
        throw std::system_error(errno, std::generic_category(), "Cannot create import staging folder");
    const fs::path stage = stagingTemplate;
    try {
        std::vector<uint8_t> buffer(1<<20);
        uint64_t done = 0;
        for (const auto& e : plan.entries) {
            fs::path target = stage;
            for (const auto& part : e.path.parent_path()) { target /= part; directory(target); }
            target /= e.path.filename();
            if (e.directory) { directory(target); continue; }
            require(!pathExists(target), "Conflicting extraction path");
            std::ofstream out(target,std::ios::binary);
            require(bool(out), "Cannot create extracted file");
            for (uint64_t offset = 0; offset < e.size;) {
                size_t count = static_cast<size_t>(std::min<uint64_t>(buffer.size(),e.size-offset));
                image.read(e.offset+offset,std::span(buffer).first(count));
                out.write(reinterpret_cast<char*>(buffer.data()),count);
                require(bool(out), "File write failed (disk full or storage unavailable)");
                offset += count; done += count;
                if (progress) progress(done,plan.bytes,e.path.generic_string());
            }
            out.close(); require(bool(out), "Cannot finish extracted file");
        }
        // Detect a changed ISO before publishing, including same-size edits to default.xbe.
        {
            const auto path = stage/"default.xbe";
            struct stat info{};
            require(stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
                    info.st_size >= 24 && info.st_size <= 64*1024*1024,
                    "Invalid extracted executable");
            std::ifstream in(path,std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
            require(!in.bad() && DarkRecomp::Native::XexCrypto::Sha256Hex(bytes) == plan.executableHash,
                    "ISO changed during extraction; executable verification failed");
        }
        {
            std::ofstream index(stage/"oogie_paths.txt", std::ios::binary);
            for (const auto& entry : plan.entries) index << entry.path.generic_string() << '\n';
            index.close();
            require(bool(index), "Cannot write game file index");
        }
        // Only publish after all game files and the index are written.
        require(!pathExists(destination), "Game folder already exists; it will not be overwritten");
        if (rename(stage.c_str(), destination.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot publish prepared game");
    } catch (...) {
        for (auto it = plan.entries.rbegin(); it != plan.entries.rend(); ++it) {
            const auto path = stage / it->path;
            if (it->directory) rmdir(path.c_str());
            else unlink(path.c_str());
        }
        unlink((stage / "oogie_paths.txt").c_str());
        rmdir(stage.c_str());
        throw;
    }
    if (progress) progress(plan.bytes,plan.bytes,"Import complete");
}

bool isReadableIso(const fs::path& path) {
    try {
        Image image(path);
        return image.size > 0;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<fs::path> findGameIsos(const std::vector<fs::path>& roots) {
    std::vector<fs::path> result;
    for (const auto& root : roots) {
        std::error_code ec;
        fs::directory_iterator it(root,ec), end;
        while (!ec && it != end) {
            const auto path = it->path();
            if (lower(path.extension().string()) == ".iso" && isReadableIso(path))
                result.push_back(path);
            if (result.size() >= 64) break;
            it.increment(ec);
        }
        if (result.size() >= 64) break;
    }
    std::sort(result.begin(),result.end());
    result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}
}
