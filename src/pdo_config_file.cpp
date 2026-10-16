#include "pdo_config.hpp"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace rocos {
namespace {
std::string readFile(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    if (input.bad()) throw std::runtime_error("cannot read " + path);
    return text;
}

struct TemporaryFile {
    std::string path;
    int fd{-1};
    ~TemporaryFile() {
        if (fd >= 0) close(fd);
        if (!path.empty()) unlink(path.c_str());
    }
};

std::string emitEntries(const std::vector<PdoEntryConfig> &entries) {
    YAML::Emitter out;
    out << YAML::Flow << YAML::BeginSeq;
    for (const auto &entry : entries) {
        out << YAML::Flow << YAML::BeginMap
            << YAML::Key << "name" << YAML::Value << entry.name
            << YAML::Key << "index" << YAML::Value << YAML::Hex << unsigned(entry.index)
            << YAML::Key << "sub_index" << YAML::Value << YAML::Dec << unsigned(entry.sub_index)
            << YAML::Key << "bit_length" << YAML::Value << unsigned(entry.bit_length)
            << YAML::EndMap;
    }
    out << YAML::EndSeq;
    if (!out.good()) throw std::runtime_error("cannot serialize discovered entries");
    return out.c_str();
}
} // namespace

bool saveDiscoveredPdoConfig(const std::string &path, const PdoBusConfig &original,
                             PdoBusConfig &resolved, std::string &error) noexcept {
    try {
        error.clear();
        const auto target = std::filesystem::canonical(path).string();
        const auto &source = original.source_yaml;
        if (source.empty() || readFile(target) != source)
            throw std::runtime_error("configuration changed since loading; refusing writeback");
        struct stat before{};
        if (stat(target.c_str(), &before) != 0 || !S_ISREG(before.st_mode))
            throw std::runtime_error("configuration is not a regular file");
        const auto root = YAML::Load(source);
        struct Edit { std::size_t begin, end; std::string text; };
        std::vector<Edit> edits;
        if (original.slaves.size() != resolved.slaves.size())
            throw std::runtime_error("discovery changed slave count");
        for (std::size_t s = 0; s < original.slaves.size(); ++s) {
            for (const bool rx : {true, false}) {
                const auto &old_pdos = rx ? original.slaves[s].rx_pdos : original.slaves[s].tx_pdos;
                const auto &new_pdos = rx ? resolved.slaves[s].rx_pdos : resolved.slaves[s].tx_pdos;
                const auto nodes = root["slaves"][s][rx ? "rx_pdos" : "tx_pdos"];
                if (old_pdos.size() != new_pdos.size())
                    throw std::runtime_error("discovery changed PDO count");
                for (std::size_t p = 0; p < old_pdos.size(); ++p) {
                    if (!old_pdos[p].entries.empty()) continue;
                    if (old_pdos[p].index != new_pdos[p].index)
                        throw std::runtime_error("discovery changed PDO index");
                    std::size_t key = std::string::npos;
                    for (const auto &field : nodes[p]) {
                        if (field.first.as<std::string>() == "entries") key = field.first.Mark().pos;
                    }
                    auto begin = source.find(':', key);
                    if (begin == std::string::npos) throw std::runtime_error("cannot locate entries field");
                    ++begin;
                    while (begin < source.size() && (source[begin] == ' ' || source[begin] == '\t')) ++begin;
                    auto end = begin;
                    if (begin < source.size() && (source[begin] == '[' || source[begin] == '{')) {
                        const char closing = source[begin] == '[' ? ']' : '}';
                        end = source.find(closing, begin) + 1;
                        if (end == 0) throw std::runtime_error("cannot locate empty entries end");
                    } else {
                        while (end < source.size() && source[end] != ' ' && source[end] != '\t' &&
                               source[end] != '\r' && source[end] != '\n' && source[end] != '#' &&
                               source[end] != ',' && source[end] != '}') ++end;
                        const auto token = source.substr(begin, end - begin);
                        if (!token.empty() && token != "null" && token != "Null" && token != "NULL" &&
                            token != "~" && token.front() != '*')
                            throw std::runtime_error("use plain [], {}, null or entries: for discovery writeback");
                    }
                    edits.push_back({begin, end, (begin > 0 && source[begin - 1] == ':' ? " " : "") +
                        emitEntries(new_pdos[p].entries) +
                        (begin == end && begin < source.size() && source[begin] == '#' ? " " : "")});
                }
            }
        }
        if (edits.empty()) return true;
        std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.begin > b.begin; });
        std::string output = source;
        std::size_t last = source.size();
        for (const auto &edit : edits) {
            if (edit.end > last) throw std::runtime_error("overlapping YAML aliases cannot be rewritten");
            output.replace(edit.begin, edit.end - edit.begin, edit.text);
            last = edit.begin;
        }
        // Create beside the target so rename is atomic and never crosses filesystems.
        std::string pattern = target + ".pdo-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        TemporaryFile temp;
        temp.fd = mkstemp(name.data());
        if (temp.fd < 0) throw std::runtime_error("cannot create writeback file: " + std::string(std::strerror(errno)));
        temp.path = name.data();
        struct stat created{};
        if (fstat(temp.fd, &created) != 0 ||
            ((created.st_uid != before.st_uid || created.st_gid != before.st_gid) &&
             fchown(temp.fd, before.st_uid, before.st_gid) != 0) ||
            fchmod(temp.fd, before.st_mode & 0777) != 0)
            throw std::runtime_error("cannot preserve configuration ownership/permissions");
        for (std::size_t offset = 0; offset < output.size();) {
            const auto count = write(temp.fd, output.data() + offset, output.size() - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw std::runtime_error("cannot write discovered configuration");
            offset += static_cast<std::size_t>(count);
        }
        if (fsync(temp.fd) != 0) throw std::runtime_error("cannot flush discovered configuration");
        PdoBusConfig checked;
        std::string validation_error;
        if (!loadPdoConfig(temp.path, checked, validation_error)) throw std::runtime_error(validation_error);
        struct stat current{};
        if (std::filesystem::canonical(path).string() != target || stat(target.c_str(), &current) != 0 ||
            current.st_dev != before.st_dev || current.st_ino != before.st_ino || readFile(target) != source)
            throw std::runtime_error("configuration changed during discovery; refusing writeback");
        if (rename(temp.path.c_str(), target.c_str()) != 0)
            throw std::runtime_error("cannot replace configuration: " + std::string(std::strerror(errno)));
        temp.path.clear();
        resolved.source_yaml = std::move(output);
        return true;
    } catch (const std::exception &e) {
        error = path + ": " + e.what();
        return false;
    }
}
} // namespace rocos
