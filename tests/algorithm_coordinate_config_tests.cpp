#include "config/LocalConfig.h"
#include "test_support.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void TestEnabledBuildPersistsAlgorithmCoordinateSetting() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "lengjing_algorithm_coordinate_config_test.json";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);

    lengjing::config::LocalConfig config(path.string());
    lengjing::ui::UiModel saved;
    saved.visual.algorithmDecrypt = true;
    std::string error;
    REQUIRE(config.Save(saved, &error));
    REQUIRE(error.empty());
    REQUIRE(ReadText(path).find("\"algorithm_decrypt\": true") !=
        std::string::npos);

    lengjing::ui::UiModel loaded;
    REQUIRE(!loaded.visual.algorithmDecrypt);
    REQUIRE(config.Load(loaded, &error));
    REQUIRE(error.empty());
    REQUIRE(loaded.visual.algorithmDecrypt);

    saved.visual.algorithmDecrypt = false;
    REQUIRE(config.Save(saved, &error));
    loaded.visual.algorithmDecrypt = true;
    REQUIRE(config.Load(loaded, &error));
    REQUIRE(!loaded.visual.algorithmDecrypt);

    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".tmp", ignored);
}

}  // namespace

int main() {
    try {
        TestEnabledBuildPersistsAlgorithmCoordinateSetting();
        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "%s\n", exception.what());
        return 1;
    }
}
