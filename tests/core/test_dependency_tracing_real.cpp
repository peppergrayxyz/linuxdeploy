#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include "linuxdeploy/core/appdir.h"
#include "linuxdeploy/core/elf_file.h"
#include "linuxdeploy/subprocess/subprocess.h"
#include "test_util.h"
#include "dependency_tracing_report.h"

namespace fs = std::filesystem;
using namespace linuxdeploy::core::elf_file;
using testing::Contains;
using testing::Not;

namespace {
    class DependencyTracingTestReal : public testing::Test {
    protected:
        fs::path directory, executable, simple, parent, leaf;
        std::optional<std::string> oldLibraryPath;

        void SetUp() override {
            if (const auto* path = getenv("LD_LIBRARY_PATH"))
                oldLibraryPath = path;
            ASSERT_EQ(unsetenv("LD_LIBRARY_PATH"), 0);
            directory = make_temporary_directory(
                (fs::temp_directory_path() / "linuxdeploy-ldd-tests-real-XXXXXX").string());
            executable = directory / "simple_executable";
            simple = directory / fs::path(SIMPLE_LIBRARY_PATH).filename();
            parent = directory / fs::path(DEPENDENCY_PARENT_LIBRARY_PATH).filename();
            leaf = directory / fs::path(DEPENDENCY_LEAF_LIBRARY_PATH).filename();
            fs::copy_file(SIMPLE_EXECUTABLE_PATH, executable);
            fs::copy_file(SIMPLE_LIBRARY_PATH, simple);
            fs::copy_file(DEPENDENCY_PARENT_LIBRARY_PATH, parent);
            fs::copy_file(DEPENDENCY_LEAF_LIBRARY_PATH, leaf);
            // Remove build-tree search paths so deleting a copy really makes it missing.
            // Each DSO needs its own RUNPATH for transitive lookup on glibc.
            for (const auto& path : {executable, simple, parent, leaf})
                ASSERT_TRUE(ElfFile(path).setRPath("$ORIGIN"));
        }

        std::vector<fs::path> trace(const fs::path& path,
                                    const std::vector<std::string>& exclusions = {}) {
            return dependency_tracing_test::trace(path, directory, exclusions);
        }

        linuxdeploy::subprocess::subprocess_result capture(const fs::path& path) {
            return dependency_tracing_test::capture(path, directory);
        }

        bool usesMusl() {
            auto env = linuxdeploy::subprocess::get_environment();
            env["LC_ALL"] = "C";
            const auto result = linuxdeploy::subprocess::subprocess({"ldd", executable.string()}, env).run();
            // Covers ld-musl-<arch>.so.1 and Android's libc_musl.so spelling.
            return result.stdout_string().find("musl") != std::string::npos;
        }

        void TearDown() override {
            if (oldLibraryPath)
                EXPECT_EQ(setenv("LD_LIBRARY_PATH", oldLibraryPath->c_str(), 1), 0);
            else
                EXPECT_EQ(unsetenv("LD_LIBRARY_PATH"), 0);
            if (!directory.empty())
                fs::remove_all(directory);
        }
    };

    // === Dependency discovery and runtime filtering ===

    TEST_F(DependencyTracingTestReal, LibraryDependencies_FindDirectAndTransitive) {
        const auto result = linuxdeploy::subprocess::subprocess({executable.string()}).run();
        ASSERT_EQ(result.exit_code(), 0) << result.stderr_string();
        const auto dependencies = trace(executable);
        EXPECT_THAT(dependencies, Contains(simple));
        EXPECT_THAT(dependencies, Contains(parent));
        EXPECT_THAT(dependencies, Contains(leaf));
        EXPECT_THAT(trace(parent), Contains(leaf));
    }

    TEST_F(DependencyTracingTestReal, RuntimeDependencies_FilterRuntimeObjects) {
        for (const auto& path : {executable, parent}) {
            const auto dependencies = trace(path);
            EXPECT_THAT(dependencies, Contains(leaf));
            for (const auto& dependency : dependencies) {
                const auto name = dependency.filename().string();
                EXPECT_NE(name, "ldd");
                EXPECT_NE(name, "libc.so");
                EXPECT_NE(name, "libc_musl.so");
                EXPECT_NE(name.find("ld-musl-"), 0u);
                EXPECT_NE(name.find("ld-linux-"), 0u);
                EXPECT_TRUE(fs::exists(dependency)) << dependency;
            }
#ifdef __GLIBC__
            // The executable needs libc, but the parent/leaf DSOs may not:
            // their fixture functions do not call libc.
            if (path == executable) {
                EXPECT_THAT(dependencies, testing::Contains(testing::Truly([](const fs::path& dependency) {
                    return dependency.filename() == "libc.so.6";
                })));
            }
#endif
        }
    }

    // === Library names ===

    TEST_F(DependencyTracingTestReal, LibraryNames_PreserveMuslLikeNames) {
        const auto libraryDir = directory / "ld-musl-testing";
        fs::create_directory(libraryDir);
        const auto movedLeaf = libraryDir / leaf.filename();
        const auto helper = directory / "libdependency_parent-musl.so";
        fs::rename(leaf, movedLeaf);
        fs::rename(parent, helper);
        ASSERT_TRUE(ElfFile(helper).setRPath("$ORIGIN/ld-musl-testing"));
        const auto patch = linuxdeploy::subprocess::subprocess({TEST_PATCHELF_PATH,
            "--replace-needed", "libdependency_parent.so", "libdependency_parent-musl.so", executable.string()}).run();
        ASSERT_EQ(patch.exit_code(), 0) << patch.stderr_string();
        const auto dependencies = trace(executable);
        EXPECT_THAT(dependencies, Contains(helper));
        EXPECT_THAT(dependencies, Contains(movedLeaf));
    }

    // === Missing dependencies and exclusions ===

    TEST_F(DependencyTracingTestReal, MissingDependencies_RejectDirect) {
        ASSERT_TRUE(fs::remove(parent));
        EXPECT_THROW(trace(executable), DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestReal, MissingDependencies_AllowExcludedDirect) {
        ASSERT_TRUE(fs::remove(parent));
        const auto dependencies = trace(executable, {"libdependency_parent.so"});
        EXPECT_THAT(dependencies, Contains(simple));
        EXPECT_THAT(dependencies, Not(Contains(parent)));
        // libdependency_leaf is reachable only through libdependency_parent.
        EXPECT_THAT(dependencies, Not(Contains(leaf)));
    }

    TEST_F(DependencyTracingTestReal, MissingDependencies_RejectTransitive) {
        ASSERT_TRUE(fs::remove(leaf));
        EXPECT_THROW(trace(executable), DependencyNotFoundError);
        EXPECT_THROW(trace(parent), DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestReal, MissingDependencies_AllowExcludedTransitive) {
        ASSERT_TRUE(fs::remove(leaf));
        const auto dependencies = trace(executable, {"libdependency_leaf.so"});
        EXPECT_THAT(dependencies, Contains(simple));
        EXPECT_THAT(dependencies, Contains(parent));
        EXPECT_THAT(dependencies, Not(Contains(leaf)));
    }

    TEST_F(DependencyTracingTestReal, MissingDependencies_AllowWildcardExclusion) {
        ASSERT_TRUE(fs::remove(leaf));
        const auto dependencies = trace(executable, {"libdependency_leaf.so*"});
        EXPECT_THAT(dependencies, Contains(parent));
        EXPECT_THAT(dependencies, Not(Contains(leaf)));
    }

    TEST_F(DependencyTracingTestReal, MissingDependencies_RejectPartiallyExcluded) {
        ASSERT_TRUE(fs::remove(parent));
        ASSERT_TRUE(fs::remove(simple));
        EXPECT_THROW(trace(executable, {"libdependency_parent.so"}),
                     DependencyNotFoundError);
    }

    TEST_F(DependencyTracingTestReal, MissingDependencies_RejectUnrelatedExclusion) {
        ASSERT_TRUE(fs::remove(leaf));
        EXPECT_THROW(trace(executable, {"libdependency_parent.so"}),
                     DependencyNotFoundError);
    }

    // === Relocation errors ===

    TEST_F(DependencyTracingTestReal, RelocationErrors_Reject_Musl) {
        const bool musl = usesMusl();
        fs::copy_file(MISSING_SYMBOL_LIBRARY_PATH, leaf, fs::copy_options::overwrite_existing);
        const auto result = capture(executable);
        // Running the executable demonstrates a genuine unresolved symbol on either libc.
        const auto run = linuxdeploy::subprocess::subprocess({executable.string()}).run();
        EXPECT_NE(run.exit_code(), 0);
        if (!musl) {
            GTEST_SKIP() << "This loader does not use musl relocation diagnostics; raw ldd output was captured";
        }
        ASSERT_NE(result.exit_code(), 0);
        EXPECT_THAT(result.stdout_string(), testing::HasSubstr("libdependency_parent.so"));
        EXPECT_THAT(result.stderr_string(), testing::HasSubstr("_Z21dependency_leaf_valuev: symbol not found"));
        for (const auto& exclusions : {std::vector<std::string>{},
                                       std::vector<std::string>{"libdependency_leaf.so"}}) {
            SCOPED_TRACE(exclusions.empty() ? "no exclusion" : "leaf excluded");
            try {
                trace(executable, exclusions);
                FAIL() << "Expected DependencyTraceError";
            } catch (const DependencyNotFoundError& e) {
                FAIL() << "Every library exists; this is a relocation failure: " << e.what();
            } catch (const DependencyTraceError& e) {
                EXPECT_THAT(e.what(), testing::HasSubstr("stdout:"));
                EXPECT_THAT(e.what(), testing::HasSubstr("libdependency_parent.so"));
                EXPECT_THAT(e.what(), testing::HasSubstr("stderr:"));
                EXPECT_THAT(e.what(), testing::HasSubstr("_Z21dependency_leaf_valuev: symbol not found"));
            }
        }
    }

    // === Invalid programs and loader diagnostics ===

    TEST_F(DependencyTracingTestReal, InvalidPrograms_Reject) {
        const bool musl = usesMusl();
        const auto invalid = directory / "invalid-program";
        std::ofstream(invalid) << "This is not an ELF program.\n";
        const auto result = capture(invalid);
        EXPECT_NE(result.exit_code(), 0);
        if (musl)
            EXPECT_THAT(result.stderr_string(), testing::HasSubstr("Not a valid dynamic program"));
        EXPECT_THROW(ElfFile file(invalid), ElfFileParseError);
    }

    // === Static programs ===

    TEST_F(DependencyTracingTestReal, StaticPrograms_HandleLoaderDiagnostics) {
        const auto result = capture(SIMPLE_EXECUTABLE_STATIC_PATH);
        EXPECT_FALSE(ElfFile(SIMPLE_EXECUTABLE_STATIC_PATH).isDynamicallyLinked());
        EXPECT_THAT(trace(SIMPLE_EXECUTABLE_STATIC_PATH), testing::IsEmpty());
        EXPECT_NE(result.exit_code(), 0);
#ifdef __GLIBC__
        EXPECT_THAT(result.stdout_string() + result.stderr_string(),
                    testing::HasSubstr("not a dynamic executable"));
#endif
    }

    // === Deployment errors ===

    TEST_F(DependencyTracingTestReal, DeploymentErrors_ReturnFailure) {
        const auto appDirPath = directory / "AppDir";
        const auto libraryDir = appDirPath / "usr/lib";
        fs::create_directories(libraryDir);
        const auto deployed = libraryDir / parent.filename();
        fs::copy_file(parent, deployed);
        capture(deployed);
        linuxdeploy::core::appdir::AppDir appDir(appDirPath);
        EXPECT_FALSE(appDir.deployDependenciesForExistingFiles());
        if (usesMusl()) {
            fs::copy_file(MISSING_SYMBOL_LIBRARY_PATH, libraryDir / leaf.filename());
            capture(deployed);
            EXPECT_FALSE(appDir.deployDependenciesForExistingFiles());
        }
    }
}
