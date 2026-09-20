#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "linuxdeploy/core/appdir.h"
#include "linuxdeploy/core/elf_file.h"
#include "test_util.h"
#include "dependency_tracing_report.h"

namespace fs = std::filesystem;
using namespace linuxdeploy::core::elf_file;
using testing::AllOf;
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;

namespace {
    class DependencyTracingTestMock : public testing::Test {
    protected:
        fs::path directory;
        std::optional<std::string> oldPath;

        void SetUp() override {
            if (const auto* path = getenv("PATH"))
                oldPath = path;
            directory = make_temporary_directory(
                (fs::temp_directory_path() / "linuxdeploy-ldd-tests-mock-XXXXXX").string());
            ASSERT_EQ(setenv("PATH", directory.c_str(), 1), 0);
        }

        std::vector<fs::path> trace(const fs::path& path,
                                    const std::vector<std::string>& exclusions = {}) {
            return dependency_tracing_test::trace(path, "/fixture", exclusions);
        }

        void TearDown() override {
            if (oldPath)
                EXPECT_EQ(setenv("PATH", oldPath->c_str(), 1), 0);
            else
                EXPECT_EQ(unsetenv("PATH"), 0);
            fs::remove_all(directory);
        }

        void setLddOutput(const std::string& output, const std::string& error = "", int status = 0) {
            std::ofstream(directory / "stdout") << output;
            std::ofstream(directory / "stderr") << error;
            // Only shell builtins: PATH intentionally contains just our fixture.
            std::ofstream script(directory / "ldd");
            script << "#!/bin/sh\n"
                   << "[ \"$LC_ALL\" = C ] || exit 90\n"
                   << "case \"$1\" in /*) ;; *) exit 91 ;; esac\n"
                   << "fixture_dir=${0%/*}\n"
                   << "while IFS= read -r line; do printf '%s\\n' \"$line\"; done < \"$fixture_dir/stdout\"\n"
                   << "while IFS= read -r line; do printf '%s\\n' \"$line\"; done < \"$fixture_dir/stderr\" >&2\n"
                   << "exit " << status << '\n';
            script.close();
            fs::permissions(directory / "ldd", fs::perms::owner_exec, fs::perm_options::add);
        }

        void setMissingDirectGlibc() {
            setLddOutput("\tlibdependency_parent.so => not found\n");
        }

        void setMissingDirectMusl() {
            setLddOutput(
                "     /lib/ld-musl-x86-64.so.1 (0x1000)\n"
                "     libsimple_library.so => /fixture/libsimple_library.so (0x1000)\n"
                "     libc.musl-x86-64.so.1 => /lib/ld-musl-x86-64.so.1 (0x1000)\n"
                "     libc++abi.so.1 => /usr/lib/libc++abi.so.1 (0x1000)\n"
                "     libunwind.so.1 => /usr/lib/libunwind.so.1 (0x1000)\n",
                "Error loading shared library libdependency_parent.so: No such file or directory (needed by /fixture/simple_executable)\n"
                "Error relocating /fixture/simple_executable: _Z22dependency_parent_initv: symbol not found\n"
                "Error relocating /fixture/simple_executable: _Z25dependency_parent_destroyv: symbol not found\n", 127);
        }

        void setMissingTransitiveMusl() {
            setLddOutput(
                "     /lib/ld-musl-x86-64.so.1 (0x1000)\n"
                "     libsimple_library.so => /fixture/libsimple_library.so (0x1000)\n"
                "     libdependency_parent.so => /fixture/libdependency_parent.so (0x1000)\n"
                "     libc.musl-x86-64.so.1 => /lib/ld-musl-x86-64.so.1 (0x1000)\n"
                "     libc++abi.so.1 => /usr/lib/libc++abi.so.1 (0x1000)\n"
                "     libunwind.so.1 => /usr/lib/libunwind.so.1 (0x1000)\n",
                "Error loading shared library libdependency_leaf.so: No such file or directory (needed by /fixture/libdependency_parent.so)\n"
                "Error relocating /fixture/libdependency_parent.so: _Z21dependency_leaf_valuev: symbol not found\n", 127);
        }

        void setMissingDirectGentooMusl() {
            setLddOutput(
                "     /lib/ld-musl-x86-64.so.1 (0x1000)\n"
                "     libsimple_library.so => /fixture/libsimple_library.so (0x1000)\n"
                "     libpng16.so.16 => /lib/libpng16.so.16 (0x1000)\n"
                "     libz.so.1 => /lib/libz.so.1 (0x1000)\n"
                "     libjpeg.so.62 => /lib/libjpeg.so.62 (0x1000)\n"
                "     libc++.so.1 => /lib/libc++.so.1 (0x1000)\n"
                "     libc++abi.so.1 => /lib/libc++abi.so.1 (0x1000)\n"
                "     libc.so => /lib/ld-musl-x86-64.so.1 (0x1000)\n"
                "     libunwind.so.1 => /lib/libunwind.so.1 (0x1000)\n",
                "Error loading shared library libdependency_parent.so: No such file or directory (needed by /fixture/simple_executable)\n"
                "Error relocating /fixture/simple_executable: _Z22dependency_parent_initv: symbol not found\n"
                "Error relocating /fixture/simple_executable: _Z25dependency_parent_destroyv: symbol not found\n", 127);
        }

        void setMissingTransitiveGentooMuslSharedLibrary() {
            setLddOutput(
                "     ldd (0x1000)\n"
                "     libc++.so.1 => /lib/libc++.so.1 (0x1000)\n"
                "     libc++abi.so.1 => /lib/libc++abi.so.1 (0x1000)\n"
                "     libc.so => ldd (0x1000)\n"
                "     libunwind.so.1 => /lib/libunwind.so.1 (0x1000)\n",
                "Error loading shared library libdependency_leaf.so: No such file or directory (needed by /fixture/libdependency_parent.so)\n"
                "Error relocating /fixture/libdependency_parent.so: _Z21dependency_leaf_valuev: symbol not found\n", 127);
        }
    };

    // === Dependency discovery and runtime filtering ===

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_Glibc) {
        setLddOutput("\tlinux-vdso.so.1 (0x1000)\n"
                     "\tlibdependency_leaf.so => /fixture/libdependency_leaf.so (0x2000)\n"
                     "\tlibc.so.6 => /lib/libc.so.6 (0x3000)\n"
                     "\tld-linux-x86-64.so.2 => /lib64/ld-linux-x86-64.so.2 (0x4000)\n");
        EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH),
                    ElementsAre(fs::path("/fixture/libdependency_leaf.so"), fs::path("/lib/libc.so.6")));
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_GlibcWithoutLibc) {
        setLddOutput("\tlinux-vdso.so.1 (0x1000)\n"
                     "\tlibdependency_leaf.so => /fixture/libdependency_leaf.so (0x2000)\n"
                     "\t/lib/ld-linux-x86-64.so.1 (0x3000)\n");
        EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH),
                    ElementsAre(fs::path("/fixture/libdependency_leaf.so")));
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_Musl) {
        setLddOutput(
            "     /lib/ld-musl-x86-64.so.1 (0x1000)\n"
            "     libsimple_library.so => /fixture/libsimple_library.so (0x1000)\n"
            "     libdependency_parent.so => /fixture/libdependency_parent.so (0x1000)\n"
            "     libc.musl-x86-64.so.1 => /lib/ld-musl-x86-64.so.1 (0x1000)\n"
            "     libc++abi.so.1 => /usr/lib/libc++abi.so.1 (0x1000)\n"
            "     libunwind.so.1 => /usr/lib/libunwind.so.1 (0x1000)\n"
            "     libdependency_leaf.so => /fixture/libdependency_leaf.so (0x1000)\n",
            "", 0);
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_PATH),
                    ElementsAre(fs::path("/fixture/libsimple_library.so"),
                                fs::path("/fixture/libdependency_parent.so"),
                                fs::path("/usr/lib/libc++abi.so.1"),
                                fs::path("/usr/lib/libunwind.so.1"),
                                fs::path("/fixture/libdependency_leaf.so")));
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_MuslAndroid) {
        setLddOutput("\tlibc_musl.so => ldd (0x1000)\n"
                     "\tlibpthread.so.0 => /lib/libc_musl.so (0x1000)\n"
                     "\tlibdependency_leaf.so => /fixture/libdependency_leaf.so (0x2000)\n");
        EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH),
                    ElementsAre(fs::path("/fixture/libdependency_leaf.so")));
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_MuslCompatibilityAliases) {
        setLddOutput("\tlibc.so => ldd (0x1000)\n"
                    "\tlibc_musl.so => ldd (0x1000)\n"
                    "\tlibcrypt.so.1 => ldd (0x1000)\n"
                    "\tlibpthread.so.0 => ldd (0x1000)\n"
                    "\tlibresolv.so.2 => ldd (0x1000)\n"
                    "\tlibrt.so.1 => ldd (0x1000)\n"
                    "\tlibm.so.6 => ldd (0x1000)\n"
                    "\tlibdl.so.2 => ldd (0x1000)\n"
                    "\tlibutil.so.1 => ldd (0x1000)\n"
                    "\tlibxnet.so.1 => ldd (0x1000)\n");

        EXPECT_THAT(trace(SIMPLE_LIBRARY_PATH), IsEmpty());
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_MuslLddSelfReference) {
        setLddOutput("\tldd (0x1000)\n"
                     "\tlibc.so => ldd (0x1000)\n"
                     "\tlibpthread.so.0 => ldd (0x1000)\n"
                     "\tlibm.so => /usr/bin/ldd (0x1000)\n"
                     "\tlibdependency_leaf.so => /fixture/libdependency_leaf.so (0x2000)\n");
        EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH),
                    ElementsAre(fs::path("/fixture/libdependency_leaf.so")));
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_MuslLibcFilename) {
        setLddOutput("\tlibc.so => /lib/libc.so (0x1000)\n"
                     "\tlibdl.so.2 => /lib/libc.so (0x1000)\n");
        EXPECT_THAT(trace(SIMPLE_LIBRARY_PATH), IsEmpty());
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_MuslSharedLibrary) {
        setLddOutput(
            "     /lib/ld-musl-x86-64.so.1 (0x1000)\n"
            "     libdependency_leaf.so => /fixture/libdependency_leaf.so (0x1000)\n"
            "     libc.musl-x86-64.so.1 => /lib/ld-musl-x86-64.so.1 (0x1000)\n",
            "", 0);
        EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH),
                    ElementsAre(fs::path("/fixture/libdependency_leaf.so")));
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_GentooMuslExecutable) {
        setLddOutput(
            "     /lib/ld-musl-x86-64.so.1 (0x1000)\n"
            "     libsimple_library.so => /fixture/libsimple_library.so (0x1000)\n"
            "     libdependency_parent.so => /fixture/libdependency_parent.so (0x1000)\n"
            "     libpng16.so.16 => /lib/libpng16.so.16 (0x1000)\n"
            "     libz.so.1 => /lib/libz.so.1 (0x1000)\n"
            "     libjpeg.so.62 => /lib/libjpeg.so.62 (0x1000)\n"
            "     libc++.so.1 => /lib/libc++.so.1 (0x1000)\n"
            "     libc++abi.so.1 => /lib/libc++abi.so.1 (0x1000)\n"
            "     libc.so => /lib/ld-musl-x86-64.so.1 (0x1000)\n"
            "     libunwind.so.1 => /lib/libunwind.so.1 (0x1000)\n"
            "     libdependency_leaf.so => /fixture/libdependency_leaf.so (0x1000)\n",
            "", 0);
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_PATH),
                    ElementsAre(fs::path("/fixture/libsimple_library.so"),
                                fs::path("/fixture/libdependency_parent.so"),
                                fs::path("/lib/libpng16.so.16"),
                                fs::path("/lib/libz.so.1"),
                                fs::path("/lib/libjpeg.so.62"),
                                fs::path("/lib/libc++.so.1"),
                                fs::path("/lib/libc++abi.so.1"),
                                fs::path("/lib/libunwind.so.1"),
                                fs::path("/fixture/libdependency_leaf.so")));
    }

    TEST_F(DependencyTracingTestMock, RuntimeDependencies_FilterRuntimeObjects_GentooMuslSharedLibrary) {
        setLddOutput(
            "     ldd (0x1000)\n"
            "     libdependency_leaf.so => /fixture/libdependency_leaf.so (0x1000)\n"
            "     libc++.so.1 => /lib/libc++.so.1 (0x1000)\n"
            "     libc++abi.so.1 => /lib/libc++abi.so.1 (0x1000)\n"
            "     libc.so => ldd (0x1000)\n"
            "     libunwind.so.1 => /lib/libunwind.so.1 (0x1000)\n",
            "", 0);
        EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH),
                    ElementsAre(fs::path("/fixture/libdependency_leaf.so"),
                                fs::path("/lib/libc++.so.1"),
                                fs::path("/lib/libc++abi.so.1"),
                                fs::path("/lib/libunwind.so.1")));
    }

    // === Library names ===

    TEST_F(DependencyTracingTestMock, LibraryNames_PreserveMuslLikeNames) {
        setLddOutput("\tlibdependency_leaf.so => /fixture/ld-musl-testing/libdependency_leaf.so (0x1000)\n"
                     "\tlibdependency_parent-musl.so => /fixture/libdependency_parent-musl.so (0x2000)\n");
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_PATH),
                    ElementsAre(fs::path("/fixture/ld-musl-testing/libdependency_leaf.so"),
                                fs::path("/fixture/libdependency_parent-musl.so")));
    }

    TEST_F(DependencyTracingTestMock, LibraryNames_PreserveNonMuslLibraryNamedLdd) {
        setLddOutput("\tlibdependency_leaf.so => ldd (0x1000)\n");

        EXPECT_THAT(
            ElfFile(DEPENDENCY_PARENT_LIBRARY_PATH).traceDynamicDependencies(),
            ElementsAre(fs::absolute("ldd")));
    }

    TEST_F(DependencyTracingTestMock, LibraryNames_PreserveOrdinaryMuslAliases) {
        setLddOutput("\tlibpthread.so.0 => /lib/libpthread.so.0 (0x1000)\n"
                     "\tlibm.so.6 => /lib/libm.so.6 (0x2000)\n"
                     "\tlibdependency_leaf.so => /opt/libc.so (0x3000)\n");
        EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH),
                    ElementsAre(fs::path("/lib/libpthread.so.0"), fs::path("/lib/libm.so.6"),
                                fs::path("/opt/libc.so")));
    }

    // === Missing dependencies and exclusions ===

    TEST_F(DependencyTracingTestMock, MissingDependencies_RejectDirect_Glibc) {
        setMissingDirectGlibc();
        EXPECT_THROW(trace(SIMPLE_EXECUTABLE_PATH), DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_RejectDirect_Musl) {
        setMissingDirectMusl();
        EXPECT_THROW(trace(SIMPLE_EXECUTABLE_PATH), DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_RejectDirect_GentooMusl) {
        setMissingDirectGentooMusl();
        EXPECT_THROW(trace(SIMPLE_EXECUTABLE_PATH), DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_AllowExcludedDirect_Glibc) {
        setMissingDirectGlibc();
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_PATH, {"libdependency_parent.so"}), IsEmpty());
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_AllowExcludedDirect_Musl) {
        setMissingDirectMusl();
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_PATH, {"libdependency_parent.so"}),
                    ElementsAre(fs::path("/fixture/libsimple_library.so"),
                                fs::path("/usr/lib/libc++abi.so.1"),
                                fs::path("/usr/lib/libunwind.so.1")));
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_AllowExcludedDirect_GentooMusl) {
        setMissingDirectGentooMusl();
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_PATH, {"libdependency_parent.so"}),
                    ElementsAre(fs::path("/fixture/libsimple_library.so"),
                                fs::path("/lib/libpng16.so.16"),
                                fs::path("/lib/libz.so.1"),
                                fs::path("/lib/libjpeg.so.62"),
                                fs::path("/lib/libc++.so.1"),
                                fs::path("/lib/libc++abi.so.1"),
                                fs::path("/lib/libunwind.so.1")));
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_RejectTransitive_Musl) {
        setMissingTransitiveMusl();
        EXPECT_THROW(trace(SIMPLE_EXECUTABLE_PATH), DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_RejectTransitive_GentooMuslSharedLibrary) {
        setMissingTransitiveGentooMuslSharedLibrary();
        EXPECT_THROW(trace(DEPENDENCY_PARENT_LIBRARY_PATH), DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_AllowExcludedTransitive_Musl) {
        setMissingTransitiveMusl();
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_PATH, {"libdependency_leaf.so"}),
                    ElementsAre(fs::path("/fixture/libsimple_library.so"),
                                fs::path("/fixture/libdependency_parent.so"),
                                fs::path("/usr/lib/libc++abi.so.1"),
                                fs::path("/usr/lib/libunwind.so.1")));
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_AllowExcludedTransitive_GentooMuslSharedLibrary) {
        setMissingTransitiveGentooMuslSharedLibrary();
        EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH, {"libdependency_leaf.so"}),
                    ElementsAre(fs::path("/lib/libc++.so.1"),
                                fs::path("/lib/libc++abi.so.1"),
                                fs::path("/lib/libunwind.so.1")));
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_AllowWildcardExclusion_Musl) {
        setMissingTransitiveMusl();
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_PATH, {"libdependency_leaf.so*"}),
                    ElementsAre(fs::path("/fixture/libsimple_library.so"),
                                fs::path("/fixture/libdependency_parent.so"),
                                fs::path("/usr/lib/libc++abi.so.1"),
                                fs::path("/usr/lib/libunwind.so.1")));
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_RejectPartiallyExcluded_Musl) {
        setLddOutput(
            "     /lib/ld-musl-x86-64.so.1 (0x1000)\n"
            "     libc.musl-x86-64.so.1 => /lib/ld-musl-x86-64.so.1 (0x1000)\n",
            "Error loading shared library libsimple_library.so: No such file or directory (needed by /fixture/simple_executable)\n"
            "Error loading shared library libdependency_parent.so: No such file or directory (needed by /fixture/simple_executable)\n"
            "Error relocating /fixture/simple_executable: _Z11hello_worldv: symbol not found\n"
            "Error relocating /fixture/simple_executable: _Z22dependency_parent_initv: symbol not found\n"
            "Error relocating /fixture/simple_executable: _Z25dependency_parent_destroyv: symbol not found\n", 127);
        EXPECT_THROW(trace(SIMPLE_EXECUTABLE_PATH, {"libdependency_parent.so"}),
                     DependencyNotFoundError);
        EXPECT_THROW(trace(SIMPLE_EXECUTABLE_PATH, {"libsimple_library.so"}),
                     DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_RejectUnrelatedExclusion_Musl) {
        setMissingTransitiveMusl();
        EXPECT_THROW(trace(SIMPLE_EXECUTABLE_PATH, {"libdependency_parent.so"}),
                     DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_RejectUnexpectedDiagnostics_Musl) {
        setLddOutput(
            "",
            "Error loading shared library libdependency_leaf.so: No such file or directory "
            "(needed by /fixture/libdependency_parent.so)\n"
            "ldd: failed to map an unrelated object\n", 127);
        EXPECT_THROW(trace(DEPENDENCY_PARENT_LIBRARY_PATH, {"libdependency_leaf.so"}), DependencyTraceError);
    }

    TEST_F(DependencyTracingTestMock, MissingDependencies_AllowExcludedBeforeOrAfterRelocation_Musl) {
        const std::string missing =
            "Error loading shared library libdependency_leaf.so: No such file or directory "
            "(needed by /fixture/libdependency_parent.so)\n";
        const std::string relocation =
            "Error relocating /fixture/libdependency_parent.so: _Z21dependency_leaf_valuev: symbol not found\n";
        // Also cover an excluded missing library without relocation fallout.
        for (const auto& diagnostics : {missing, missing + relocation, relocation + missing}) {
            SCOPED_TRACE(diagnostics);
            setLddOutput("", diagnostics, 127);
            EXPECT_THAT(trace(DEPENDENCY_PARENT_LIBRARY_PATH, {"libdependency_leaf.so"}), IsEmpty());
        }
    }

    // === Relocation errors ===

    TEST_F(DependencyTracingTestMock, RelocationErrors_Reject_MuslWithBothStreams) {
        setLddOutput(
            "     /lib/ld-musl-x86-64.so.1 (0x1000)\n"
            "     libsimple_library.so => /fixture/libsimple_library.so (0x1000)\n"
            "     libdependency_parent.so => /fixture/libdependency_parent.so (0x1000)\n"
            "     libc.musl-x86-64.so.1 => /lib/ld-musl-x86-64.so.1 (0x1000)\n"
            "     libc++abi.so.1 => /usr/lib/libc++abi.so.1 (0x1000)\n"
            "     libunwind.so.1 => /usr/lib/libunwind.so.1 (0x1000)\n"
            "     libdependency_leaf.so => /fixture/libdependency_leaf.so (0x1000)\n",
            "Error relocating /fixture/libdependency_parent.so: _Z21dependency_leaf_valuev: symbol not found\n", 127);

        for (const auto& exclusions : {std::vector<std::string>{},
                                       std::vector<std::string>{"libdependency_leaf.so"}}) {
            SCOPED_TRACE(exclusions.empty() ? "no exclusion" : "leaf excluded");
            try {
                trace(SIMPLE_EXECUTABLE_PATH, exclusions);
                FAIL() << "Expected DependencyTraceError";
            } catch (const DependencyNotFoundError& e) {
                FAIL() << "Every library exists; this is a relocation failure: " << e.what();
            } catch (const DependencyTraceError& e) {
                EXPECT_THAT(e.what(),
                            AllOf(HasSubstr("stdout:"), HasSubstr("stderr:"),
                                  HasSubstr("libdependency_leaf.so"),
                                  HasSubstr("_Z21dependency_leaf_valuev")));
            }
        }
    }

    TEST_F(DependencyTracingTestMock, RelocationErrors_Reject_MuslWithoutStdout) {
        setLddOutput("", "Error relocating /fixture/libdependency_parent.so: _Z21dependency_leaf_valuev: symbol not found\n", 127);
        EXPECT_THROW(trace(DEPENDENCY_PARENT_LIBRARY_PATH), DependencyTraceError);
    }

    TEST_F(DependencyTracingTestMock, RelocationErrors_Reject_GentooMuslSharedLibrary) {
        setLddOutput(
            "     ldd (0x1000)\n"
            "     libdependency_leaf.so => /fixture/AppDir/usr/lib/libdependency_leaf.so (0x1000)\n"
            "     libc++.so.1 => /lib/libc++.so.1 (0x1000)\n"
            "     libc++abi.so.1 => /lib/libc++abi.so.1 (0x1000)\n"
            "     libc.so => ldd (0x1000)\n"
            "     libunwind.so.1 => /lib/libunwind.so.1 (0x1000)\n",
            "Error relocating /fixture/AppDir/usr/lib/libdependency_parent.so: _Z21dependency_leaf_valuev: symbol not found\n", 127);
        EXPECT_THROW(trace(DEPENDENCY_PARENT_LIBRARY_PATH), DependencyTraceError);
        EXPECT_THROW(trace(DEPENDENCY_PARENT_LIBRARY_PATH, {"libdependency_leaf.so"}),
                     DependencyTraceError);
    }

    // === Invalid programs and loader diagnostics ===

    TEST_F(DependencyTracingTestMock, InvalidPrograms_AcceptNonDynamicDiagnostic_Glibc) {
        // A dynamic ELF can still receive glibc's non-dynamic diagnostic.
        for (const bool onStderr : {false, true}) {
            SCOPED_TRACE(onStderr ? "stderr" : "stdout");
            setLddOutput(onStderr ? "" : "not a dynamic executable\n",
                         onStderr ? "not a dynamic executable\n" : "", 1);
            EXPECT_THAT(trace(SIMPLE_LIBRARY_PATH), IsEmpty());
        }
    }

    TEST_F(DependencyTracingTestMock, InvalidPrograms_RejectNonzeroExitWithoutStderr) {
        for (const std::string output : {"", "libdependency_leaf.so => /fixture/libdependency_leaf.so (0x1000)\n"}) {
            SCOPED_TRACE(output);
            setLddOutput(output, "", 127);
            EXPECT_THROW(trace(DEPENDENCY_PARENT_LIBRARY_PATH), DependencyTraceError);
        }
    }

    TEST_F(DependencyTracingTestMock, InvalidPrograms_Reject_Musl) {
        setLddOutput(
            "",
            "/lib/ld-musl-x86-64.so.1: /fixture/libsimple_library.so: Not a valid dynamic program\n", 1);
        EXPECT_THROW(trace(SIMPLE_LIBRARY_PATH), DependencyTraceError);
    }

    TEST_F(DependencyTracingTestMock, InvalidPrograms_Reject_GentooMusl) {
        setLddOutput(
            "",
            "ldd: /fixture/libsimple_library.so: Not a valid dynamic program\n", 1);
        EXPECT_THROW(trace(SIMPLE_LIBRARY_PATH), DependencyTraceError);
    }

    // === Static programs ===

    TEST_F(DependencyTracingTestMock, StaticPrograms_ReturnNoDependenciesWithoutLdd) {
        // PATH contains only an empty fixture directory: invoking ldd would fail.
        EXPECT_FALSE(ElfFile(SIMPLE_EXECUTABLE_STATIC_PATH).isDynamicallyLinked());
        EXPECT_THAT(ElfFile(SIMPLE_EXECUTABLE_STATIC_PATH).traceDynamicDependencies(), IsEmpty());
    }

    // === Deployment errors ===

    TEST_F(DependencyTracingTestMock, DeploymentErrors_ReturnFailure) {
        setLddOutput("", "ldd: failed to load shared object\n", 1);
        const auto appDirPath = directory / "AppDir";
        fs::create_directories(appDirPath / "usr/lib");
        fs::copy_file(SIMPLE_LIBRARY_PATH, appDirPath / "usr/lib/libsimple_library.so");
        dependency_tracing_test::capture(appDirPath / "usr/lib/libsimple_library.so", directory);
        linuxdeploy::core::appdir::AppDir appDir(appDirPath);
        EXPECT_FALSE(appDir.deployDependenciesForExistingFiles());
    }
}
