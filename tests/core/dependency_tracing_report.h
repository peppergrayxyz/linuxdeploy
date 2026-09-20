#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

#include "gtest/gtest.h"
#include "linuxdeploy/core/elf_file.h"
#include "linuxdeploy/subprocess/subprocess.h"

namespace dependency_tracing_test {
    inline std::string normalize(std::string value, const std::filesystem::path& root) {
        if (!root.empty()) {
            const auto prefix = root.string();
            for (std::size_t pos = 0; (pos = value.find(prefix, pos)) != std::string::npos;) {
                value.replace(pos, prefix.size(), "<fixture>");
                pos += std::string("<fixture>").size();
            }
        }
        return std::regex_replace(value, std::regex("0x[0-9a-fA-F]+"), "<address>");
    }

    // Run the same command/environment as ElfFile. This is a diagnostic capture;
    // the production tracer subsequently invokes ldd itself on the unchanged setup.
    inline linuxdeploy::subprocess::subprocess_result capture(
            const std::filesystem::path& path,
            const std::filesystem::path& root,
            const std::vector<std::string>& exclusions = {}) {
        namespace fs = std::filesystem;
        auto env = linuxdeploy::subprocess::get_environment();
        env["LC_ALL"] = "C";
        const auto target = fs::canonical(path);
        const auto result = linuxdeploy::subprocess::subprocess({"ldd", target.string()}, env).run();
        const auto* test = testing::UnitTest::GetInstance()->current_test_info();
        const std::string scenario = test->name();
        std::ostringstream report;

        report << "\n=== ldd capture ===\nScenario: " << scenario
               << "\nTarget: " << target.string()
               << "\nExclusions:";

        for (const auto& exclusion : exclusions) {
            report << ' ' << exclusion;
        }

        report << "\nExit status: " << result.exit_code()
               << "\nstdout (raw):\n" << result.stdout_string()
               << "\nstderr (raw):\n" << result.stderr_string()
               << "\nstdout (normalized):\n" << normalize(result.stdout_string(), root)
               << "\nstderr (normalized):\n" << normalize(result.stderr_string(), root)
               << "\n=== end ldd capture ===\n";
        
        if (std::getenv("LINUXDEPLOY_LDD_REPORT_STDOUT")) {
            std::cout << report.str() << std::flush;
        }

        if (const auto* outputDir = std::getenv("LINUXDEPLOY_LDD_REPORT_DIR")) {
            fs::create_directories(outputDir);
            const auto name = std::regex_replace(
                std::string(test->test_suite_name()) + "." + test->name() + "." + target.filename().string(),
                std::regex("[^a-zA-Z0-9_.-]"), "_");
            std::ofstream file(fs::path(outputDir) / (name + ".txt"), std::ios::app);
            file << report.str();
            if (!file)
                throw std::runtime_error("Could not write ldd capture: " + name);
        }
        return result;
    }

    inline std::vector<std::filesystem::path> trace(
            const std::filesystem::path& path,
            const std::filesystem::path& root,
            const std::vector<std::string>& exclusions = {}) {
        capture(path, root, exclusions);
        return linuxdeploy::core::elf_file::ElfFile(path).traceDynamicDependencies(exclusions);
    }
}
