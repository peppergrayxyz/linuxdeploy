// system headers
#include <algorithm>
#include <assert.h>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <sys/mman.h>
#include <utility>

// local headers
#include "linuxdeploy/core/elf_file.h"
#include "linuxdeploy/log/log.h"
#include "linuxdeploy/util/util.h"
#include "linuxdeploy/subprocess/subprocess.h"

// older C libraries' headers lack this flag
#ifndef DF_1_PIE
#define DF_1_PIE 0x08000000
#endif

using namespace linuxdeploy::log;

namespace fs = std::filesystem;

namespace linuxdeploy {
    namespace core {
        namespace elf_file {
            class ElfFile::PrivateData {
                public:
                    const fs::path path;
                    uint8_t elfClass = ELFCLASSNONE;
                    uint8_t elfABI = 0;
                    uint16_t elfMachine = EM_NONE;
                    bool isDebugSymbolsFile = false;
                    bool isDynamicallyLinked = false;
                    fs::path interpreter;

                public:
                    explicit PrivateData(fs::path path) : path(std::move(path)) {}

                public:
                    static std::string getPatchelfPath() {
                        // by default, try to use a patchelf next to the linuxdeploy binary
                        // if that isn't available, fall back to searching for patchelf in the PATH
                        std::string patchelfPath;

                        const auto envPatchelf = getenv("PATCHELF");

                        // allows users to use a custom patchelf instead of the bundled one
                        if (envPatchelf != nullptr) {
                            ldLog() << LD_DEBUG << "Using patchelf specified in $PATCHELF:" << envPatchelf << std::endl;
                            patchelfPath = envPatchelf;
                        } else {
                            auto binDirPath = fs::path(util::getOwnExecutablePath());
                            auto localPatchelfPath = binDirPath.parent_path() / "patchelf";

                            if (fs::exists(localPatchelfPath)) {
                                patchelfPath = localPatchelfPath.string();
                            } else {
                                for (const fs::path directory : util::split(getenv("PATH"), ':')) {
                                    if (!fs::is_directory(directory))
                                        continue;

                                    auto path = directory / "patchelf";

                                    if (fs::is_regular_file(path)) {
                                        patchelfPath = path.string();
                                        break;
                                    }
                                }
                            }
                        }

                        if (!fs::is_regular_file(patchelfPath)) {
                            ldLog() << LD_ERROR << "Could not find patchelf: no such file:" << patchelfPath << std::endl;
                            throw std::runtime_error("Could not find patchelf");
                        }

                        ldLog() << LD_DEBUG << "Using patchelf:" << patchelfPath << std::endl;
                        return patchelfPath;
                    }

                private:
                    template<typename Ehdr_T, typename Shdr_T, typename Phdr_T, typename Dyn_T>
                    void parseElfHeader(std::shared_ptr<uint8_t> data, size_t dataSize) {
                        // TODO: the following code will _only_ work if the native byte order equals the program's
                        // this should not be a big problem as we don't offer ARM builds yet, and require the user to
                        // use a matching binary for the target binaries

                        auto* ehdr = reinterpret_cast<Ehdr_T*>(data.get());

                        elfABI = ehdr->e_ident[EI_OSABI];
                        elfMachine = ehdr->e_machine;

                        std::vector<Shdr_T> sections;

                        // parse section header table
                        // first, we collect all entries in a vector so we can conveniently iterate over it
                        for (uint64_t i = 0; i < ehdr->e_shnum; ++i) {
                            auto* nextShdr = reinterpret_cast<Shdr_T*>(data.get() + ehdr->e_shoff + i * sizeof(Shdr_T));
                            sections.emplace_back(*nextShdr);
                        }

                        auto getString = [data, &sections, ehdr](uint64_t offset) {
                            assert(ehdr->e_shstrndx != SHN_UNDEF);
                            const auto& stringTableSection = sections[ehdr->e_shstrndx];
                            return std::string{reinterpret_cast<char*>(data.get() + stringTableSection.sh_offset + offset)};
                        };

                        // now that we can look up texts, we can create a map to easily access the sections by name
                        std::unordered_map<std::string, Shdr_T> sectionsMap;
                        std::for_each(sections.begin(), sections.end(), [&sectionsMap, &getString](const Shdr_T& shdr) {
                            const auto headerName = getString(shdr.sh_name);
                            sectionsMap.insert(std::make_pair(headerName, shdr));
                        });

                        // this function is based on observations of the behavior of:
                        // - strip --only-keep-debug
                        // - objcopy --only-keep-debug
                        isDebugSymbolsFile = (sectionsMap[".text"].sh_type == SHT_NOBITS);

                        // https://stackoverflow.com/a/7298931
                        const Phdr_T* dynamicPhdr = nullptr;
                        for (uint64_t i = 0; i < ehdr->e_phnum; ++i) {
                            auto* nextPhdr = reinterpret_cast<Phdr_T*>(data.get() + ehdr->e_phoff + i * sizeof(Phdr_T));
                            switch (nextPhdr->p_type) {
                                case PT_INTERP:
                                    // debug symbols files retain PT_INTERP, but its contents are stripped, so it may
                                    // point to unrelated data or beyond the end of the file
                                    if (nextPhdr->p_filesz > 0 && nextPhdr->p_offset < dataSize) {
                                        const auto* interp = reinterpret_cast<const char*>(data.get() + nextPhdr->p_offset);
                                        const auto maxSize = std::min<uint64_t>(nextPhdr->p_filesz, dataSize - nextPhdr->p_offset);
                                        interpreter = std::string{interp, strnlen(interp, maxSize)};
                                    }
                                    isDynamicallyLinked = true;
                                    break;
                                case PT_DYNAMIC:
                                    dynamicPhdr = nextPhdr;
                                    break;
                            }
                        }

                        // shared libraries have no PT_INTERP, but a PT_DYNAMIC segment
                        // static-pie executables (e.g., the default for Rust's musl targets) look the same, but need no
                        // other libraries, and are marked as PIE by the linker, which shared libraries never are
                        if (!isDynamicallyLinked && dynamicPhdr != nullptr)
                            isDynamicallyLinked = isDebugSymbolsFile || !isStaticPie<Dyn_T>(data, dataSize, dynamicPhdr);
                    }

                    // the caller must make sure the file has no PT_INTERP
                    // not suitable for debug symbols files, whose dynamic section has been stripped, so the segment
                    // points to unrelated data or beyond the end of the file
                    template<typename Dyn_T, typename Phdr_T>
                    static bool isStaticPie(const std::shared_ptr<uint8_t>& data, size_t dataSize, const Phdr_T* dynamicPhdr) {
                        if (dynamicPhdr->p_offset >= dataSize)
                            return false;

                        bool isPie = false;

                        const auto dynamicSize = std::min<uint64_t>(dynamicPhdr->p_filesz, dataSize - dynamicPhdr->p_offset);
                        const auto dynamicEntries = dynamicSize / sizeof(Dyn_T);
                        for (uint64_t i = 0; i < dynamicEntries; ++i) {
                            auto* dyn = reinterpret_cast<Dyn_T*>(data.get() + dynamicPhdr->p_offset + i * sizeof(Dyn_T));

                            if (dyn->d_tag == DT_NULL)
                                break;

                            // a PIE which needs other libraries but has no PT_INTERP (e.g., linked with
                            // --no-dynamic-linker) must still be traced
                            if (dyn->d_tag == DT_NEEDED)
                                return false;

                            if (dyn->d_tag == DT_FLAGS_1 && (dyn->d_un.d_val & DF_1_PIE) != 0)
                                isPie = true;
                        }

                        return isPie;
                    }

                    // multilib systems can have loaders for several architectures installed
                    bool isLoaderForThisFile(const fs::path& loader) const {
                        std::ifstream ifs(loader.string(), std::ios::binary);
                        unsigned char header[EI_NIDENT + 4]{};
                        if (!ifs.read(reinterpret_cast<char*>(header), sizeof(header)))
                            return false;

                        // e_machine directly follows e_type, and both have the same offset in 32-bit and 64-bit files
                        uint16_t machine;
                        std::memcpy(&machine, header + EI_NIDENT + 2, sizeof(machine));

                        return std::memcmp(header, ELFMAG, SELFMAG) == 0 && header[EI_CLASS] == elfClass && machine == elfMachine;
                    }

                public:
                    // find a dynamic loader which can list the dependencies like ldd does
                    // used as a fallback on systems which do not ship ldd (e.g., Alpine without musl-utils)
                    fs::path findLoader() const {
                        // executables tell us which loader to use
                        // the loader will be executed, so only trust loaders installed to the system's library
                        // directories rather than any path an ELF file references
                        static const std::vector<fs::path> systemLoaderDirs{
                            "/lib", "/lib32", "/lib64", "/libx32", "/libexec", "/usr/lib", "/usr/lib32", "/usr/lib64",
                        };

                        std::error_code ec;
                        if (!interpreter.empty() && fs::exists(interpreter, ec)
                            && std::find(systemLoaderDirs.begin(), systemLoaderDirs.end(), interpreter.parent_path()) != systemLoaderDirs.end()
                            && isLoaderForThisFile(interpreter))
                            return interpreter;

                        // shared libraries do not reference a loader, but musl's loader is installed to a fixed location
                        // glibc systems always ship ldd, so there is no need to look for glibc's loader
                        std::vector<fs::path> muslLoaders;
                        for (fs::directory_iterator it("/lib", ec), end; !ec && it != end; it.increment(ec)) {
                            const auto fileName = it->path().filename().string();
                            if (util::stringStartsWith(fileName, "ld-musl-") && util::stringEndsWith(fileName, ".so.1")
                                && isLoaderForThisFile(it->path()))
                                muslLoaders.emplace_back(it->path());
                        }

                        // with multiple matching loaders installed (e.g., arm and armhf), we cannot tell which one is the right one
                        if (muslLoaders.size() == 1)
                            return muslLoaders.front();

                        return {};
                    }

                public:
                    void readDataUsingElfAPI() {
                        int fd = open(path.c_str(), O_RDONLY);
                        auto map_size = static_cast<size_t>(lseek(fd, 0, SEEK_END));

                        std::shared_ptr<uint8_t> data(
                            static_cast<uint8_t*>(mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0)),
                            [map_size](uint8_t* p) {
                                if (munmap(static_cast<void*>(p), map_size) != 0) {
                                    int error = errno;
                                    throw ElfFileParseError(std::string("Failed to call munmap(): ") + strerror(error));
                                }
                                p = nullptr;
                            }
                        );
                        close(fd);

                        // check which ELF "class" (32-bit or 64-bit) to use
                        // the "class" is available at a specific constant offset in the section e_ident, which
                        // happens to be the first section, so just reading one byte at EI_CLASS yields the data we're
                        // looking for
                        elfClass = data.get()[EI_CLASS];

                        switch (elfClass) {
                            case ELFCLASS32:
                                parseElfHeader<Elf32_Ehdr, Elf32_Shdr, Elf32_Phdr, Elf32_Dyn>(data, map_size);
                                break;
                            case ELFCLASS64:
                                parseElfHeader<Elf64_Ehdr, Elf64_Shdr, Elf64_Phdr, Elf64_Dyn>(data, map_size);
                                break;
                            default:
                                throw ElfFileParseError("Unknown ELF class: " + std::to_string(elfClass));
                        }
                    }
            };

            ElfFile::ElfFile(const std::filesystem::path& path) {
                // check if file exists
                if (!fs::exists(path))
                    throw ElfFileParseError("No such file or directory: " + path.string());

                // check magic bytes
                std::ifstream ifs(path.string());
                if (!ifs)
                    throw ElfFileParseError("Could not open file: " + path.string());

                std::vector<char> magicBytes(4);
                ifs.read(magicBytes.data(), 4);

                if (strncmp(magicBytes.data(), "\177ELF", 4) != 0)
                    throw ElfFileParseError("Invalid magic bytes in file header");

                d = new PrivateData(path);
                d->readDataUsingElfAPI();
            }

            ElfFile::~ElfFile() {
                delete d;
            }

            std::vector<fs::path> ElfFile::traceDynamicDependencies(const std::vector<std::string>& excludeLibraryPatterns) {
                // this method's purpose is to abstract this process
                // the caller doesn't care _how_ it's done, after all

                // for now, we use the same ldd based method linuxdeployqt uses

                // Static ELF files have no dynamic dependencies and need no loader probe.
                if (!isDynamicallyLinked())
                    return {};

                std::vector<fs::path> paths;

                auto env = subprocess::get_environment();
                env["LC_ALL"] = "C";

                // workaround for https://sourceware.org/bugzilla/show_bug.cgi?id=25263
                // when you pass an absolute path to ldd, it can find libraries referenced in the rpath properly
                // this bug was first found when trying to find a library next to the binary which contained $ORIGIN
                // note that this is just a bug in ldd, the linker has always worked as intended
                const auto resolvedPath = fs::canonical(d->path);

                std::vector<std::string> lddCommand{"ldd", resolvedPath.string()};

                if (util::which("ldd").empty()) {
                    const auto loader = d->findLoader();

                    if (loader.empty())
                        throw DependencyTraceError("Could not find ldd or a dynamic loader to trace dependencies of " + resolvedPath.string());

                    // both loaders produce ldd compatible output, but must be asked differently
                    // calling a loader which ignores these would run the program, therefore only known ones are used
                    const auto loaderName = loader.filename().string();
                    if (util::stringStartsWith(loaderName, "ld-musl-")) {
                        // musl does not support LD_TRACE_LOADED_OBJECTS
                        lddCommand = {loader.string(), "--list", resolvedPath.string()};
                    } else if (util::stringStartsWith(loaderName, "ld-linux") || util::stringStartsWith(loaderName, "ld64.so")
                               || util::stringStartsWith(loaderName, "ld.so") || util::stringStartsWith(loaderName, "ld-elf")) {
                        // glibc's --list stops at the first missing library, while this mode (used by glibc's ldd)
                        // reports all of them
                        // FreeBSD's ld-elf.so.1 supports the same variable
                        env["LD_TRACE_LOADED_OBJECTS"] = "1";
                        lddCommand = {loader.string(), resolvedPath.string()};
                    } else {
                        throw DependencyTraceError("Could not find ldd, and unknown dynamic loader " + loader.string() +
                                                   " cannot be used to trace dependencies of " + resolvedPath.string());
                    }

                    ldLog() << LD_WARNING << "Could not find ldd in $PATH, using dynamic loader" << loader << "instead" << std::endl;
                }

                subprocess::subprocess lddProc(lddCommand, env);

                const auto result = lddProc.run();

                if (result.exit_code() != 0) {
                    if (result.stdout_string().find("not a dynamic executable") != std::string::npos || result.stderr_string().find("not a dynamic executable") != std::string::npos) {
                        ldLog() << LD_WARNING << this->d->path << "is not linked dynamically" << std::endl;
                        return {};
                    }

                    // musl reports missing DT_NEEDED libraries on stderr and
                    // returns a non-zero status, while still printing the
                    // dependencies it could resolve on stdout.
                    //
                    // Accept excluded missing libraries and relocation errors,
                    // but reject all other failures. musl also checks symbols
                    // in this mode (unlike glibc's ldd without -r), which fails
                    // for plugins whose symbols are provided by the host program.
                    // other load errors (e.g., a library for another architecture) are no missing libraries, and must
                    // not be accepted by excluding the library
                    static const std::regex muslMissingLibraryExpr(
                        R"(^Error loading shared library ([^:]+): No such file or directory(?: \(needed by .+\))?$)");
                    static const std::regex muslMissingSymbolExpr(
                        R"(^Error relocating .+: .+: symbol not found$)");

                    bool sawExcludedMissingLibrary = false;
                    bool sawMissingSymbolExpr = false;
                    bool sawUnexpectedError = false;

                    for (auto& line : util::splitLines(result.stderr_string())) {
                        util::trim(line);

                        if (line.empty())
                            continue;

                        std::smatch match;
                        if (std::regex_match(line, match, muslMissingLibraryExpr)) {
                            auto missingLibrary = match[1].str();
                            util::trim(missingLibrary);

                            if (!util::isInExcludelist(missingLibrary, excludeLibraryPatterns)) {
                                throw DependencyNotFoundError("Could not find dependency: " + missingLibrary);
                            }

                            sawExcludedMissingLibrary = true;
                            ldLog() << LD_WARNING
                                    << resolvedPath.string()
                                    << "depends on excluded library:"
                                    << missingLibrary << std::endl;

                            continue;
                        }

                        if (std::regex_match(line, muslMissingSymbolExpr)) {
                            sawMissingSymbolExpr = true;
                        } else {
                            sawUnexpectedError = true;
                        }
                    }

                    // A nonzero exit without a known diagnostic is still a
                    // failure, even if stderr is empty. Check after scanning
                    // so diagnostic order does not affect the result.
                    if (sawUnexpectedError || (!sawExcludedMissingLibrary && !sawMissingSymbolExpr)) {
                        std::ostringstream message;
                        message << "Failed to run ldd on " << resolvedPath.string()
                                << ": exited with code " << result.exit_code();
                        if (!result.stdout_string().empty())
                            message << "\nstdout:\n" << result.stdout_string();
                        if (!result.stderr_string().empty())
                            message << "\nstderr:\n" << result.stderr_string();
                        throw DependencyTraceError(message.str());
                    }

                    // all libraries were found, so the dependency list is complete, but the program may still fail
                    // at runtime if the symbols are not provided by someone else
                    if (sawMissingSymbolExpr) {
                        ldLog() << LD_WARNING << resolvedPath.string() << LD_NO_SPACE
                                << ": ignoring unresolved symbols, they must be provided at runtime (e.g., by an excluded "
                                   "library or the program loading this file):" << std::endl
                                << result.stderr_string();
                    }
                }

                // FreeBSD appends an address to missing libraries, so check these
                // records before the expression for successfully resolved paths.
                static const std::regex missingLibraryExpr(R"(\s*(.+)\s+=>\s+not found(?:\s+\([^)]+\))?\s*)");
                static const std::regex expr(R"(\s*(.+)\s+\=>\s+(.+)\s+\((.+)\)\s*)");
                auto lddLines = util::splitLines(result.stdout_string());

                // filter known-problematic, known-unneeded lines
                // see https://github.com/linuxdeploy/linuxdeploy/issues/210
                lddLines.erase(
                    std::remove_if(lddLines.begin(), lddLines.end(), [&lddLines](auto line) {
                        if (util::stringContains(line, "linux-vdso.so") || util::stringContains(line, "ld-linux-")) {
                            ldLog() << LD_DEBUG << "skipping linker related object" << line << std::endl;
                            return true;
                        }

                        return false;
                    }),
                    lddLines.end()
                );

                // These names are aliases for musl's combined libc/dynamic loader.
                static const std::regex muslLibcExpr(R"(lib(c|c_musl|crypt|pthread|resolv|rt|m|dl|util|xnet)\..+)");

                std::smatch what;
                for (const auto& line : lddLines) {
                    if (std::regex_match(line, what, missingLibraryExpr)) {
                        auto missingLib = what[1].str();
                        util::trim(missingLib);
                        util::trim(missingLib, '\t');
                        if (!util::isInExcludelist(missingLib, excludeLibraryPatterns)) {
                            throw DependencyNotFoundError("Could not find dependency: " + missingLib);
                        }
                        ldLog() << LD_WARNING << resolvedPath.string() << "depends on excluded library:" << missingLib << std::endl;
                        continue;
                    }

                    if (std::regex_search(line, what, expr)) {
                        auto libraryName = what[1].str();
                        util::trim(libraryName);
                        auto libraryPath = what[2].str();
                        util::trim(libraryPath);

                        const auto fileName = fs::path(libraryPath).filename().string();
                        const bool muslLoader = util::stringStartsWith(fileName, "ld-musl-")
                                                && util::stringEndsWith(fileName, ".so.1");
                        // When a shared object has no PT_INTERP, musl can identify
                        // itself as argv[0] ("ldd") or by its libc.so filename.
                        // Neither is an ordinary library to copy into the AppDir.
                        const bool muslSelfReference = (fileName == "ldd"
                                                        || fileName == "libc.so"
                                                        || fileName == "libc_musl.so")
                                                       && std::regex_match(libraryName, muslLibcExpr);

                        if (muslLoader || muslSelfReference) {
                            ldLog() << LD_DEBUG << "skipping musl runtime object" << line << std::endl;
                            continue;
                        }

                        paths.push_back(fs::absolute(libraryPath));
                    } else {
                        ldLog() << LD_DEBUG << "Invalid ldd output: " << line << std::endl;
                    }
                }

                return paths;
            }

            std::string ElfFile::getRPath() {
                // don't try to fetch patchelf path in a catchall to make sure the process exists when the tool cannot be found
                const auto patchelfPath = PrivateData::getPatchelfPath();

                try {
                    subprocess::subprocess patchelfProc({patchelfPath, "--print-rpath", d->path.string()});

                    const auto result = patchelfProc.run();

                    if (result.exit_code() != 0) {
                        // if file is not an ELF executable, there is no need for a detailed error message
                        if (result.exit_code() == 1 && result.stderr_string().find("not an ELF executable") != std::string::npos) {
                            return "";
                        } else {
                            ldLog() << LD_ERROR << "Call to patchelf failed:" << std::endl << result.stderr_string();
                            return "";
                        }
                    }

                    auto stdoutContents = result.stdout_string();

                    util::trim(stdoutContents, '\n');
                    util::trim(stdoutContents);

                    return stdoutContents;
                } catch (const std::exception&) {
                    return "";
                }
            }

            bool ElfFile::setRPath(const std::string& value) {
                // don't try to fetch patchelf path in a catchall to make sure the process exists when the tool cannot be found
                const auto patchelfPath = PrivateData::getPatchelfPath();

                // calling (older versions of) patchelf on symlinks can lead to weird behavior, e.g., patchelf copying the
                // original file and patching the copy instead of patching the symlink target
                const auto canonicalPath = fs::canonical(d->path);

                ldLog() << LD_DEBUG << "Calling patchelf on canonical path" << canonicalPath << "instead of original path" << d->path << std::endl;

                try {
                    subprocess::subprocess patchelfProc({patchelfPath.c_str(), "--set-rpath", value.c_str(), canonicalPath.c_str()});

                    const auto result = patchelfProc.run();

                    if (result.exit_code() != 0) {
                        ldLog() << LD_ERROR << "Call to patchelf failed:" << std::endl << result.stderr_string() << std::endl;
                        return false;
                    }
                } catch (const std::exception&) {
                    return false;
                }

                return true;
            }

            uint8_t ElfFile::getSystemElfABI() {
                // the only way to get the system's ELF ABI is to read the own executable using the ELF header,
                // and get the ELFOSABI flag
                auto self = std::shared_ptr<char>(realpath("/proc/self/exe", nullptr), [](char* p) { free(p); });

                if (self == nullptr)
                    throw ElfFileParseError("Could not read /proc/self/exe");

                std::ifstream ifs(self.get());

                if (!ifs)
                    throw ElfFileParseError("Could not open file: " + std::string(self.get()));

                // the "class" is available at a specific constant offset in the section e_ident, which
                // happens to be the first section, so just reading one byte at EI_CLASS yields the data we're
                // looking for
                ifs.seekg(EI_OSABI);

                char buf;
                ifs.read(&buf, 1);

                return static_cast<uint8_t>(buf);
            }

            uint8_t ElfFile::getSystemElfClass() {
                #if __SIZEOF_POINTER__ == 4
                return ELFCLASS32;
                #elif __SIZEOF_POINTER__ == 8
                return ELFCLASS64;
                #else
                #error "Invalid address size"
                #endif
            }

            uint8_t ElfFile::getSystemElfEndianness() {
                #if __BYTE_ORDER == __LITTLE_ENDIAN
                return ELFDATA2LSB;
                #elif __BYTE_ORDER == __BIG_ENDIAN
                return ELFDATA2MSB;
                #else
                #error "Unknown machine endianness"
                #endif
            }

            uint8_t ElfFile::getElfClass()  {
                return d->elfClass;
            }

            uint8_t ElfFile::getElfABI() {
                return d->elfABI;
            }

            bool ElfFile::isDebugSymbolsFile() {
                return d->isDebugSymbolsFile;
            }

            bool ElfFile::isDynamicallyLinked() {
                return d->isDynamicallyLinked;
            }
        }
    }
}
