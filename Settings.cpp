#include "Settings.h"

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace
{
    inline const std::filesystem::path kConfigDir = std::filesystem::path("Data") / "SKSE" / "Plugins";
    inline const std::filesystem::path kConfigPath = kConfigDir / "ArmorResistsMagic.ini";

    constexpr int kDefaultLogLevel = 1;

    int ClampLogLevel(int level)
    {
        return std::clamp(level, 0, 2);
    }
}

namespace SETTINGS
{
    int LoadLogLevel()
    {
        std::error_code ec;
        std::filesystem::create_directories(kConfigDir, ec);

        std::ifstream file(kConfigPath);
        if (!file.is_open()) {
            SaveLogLevel(kDefaultLogLevel);
            return kDefaultLogLevel;
        }

        int level = kDefaultLogLevel;
        file >> level;

        if (file.fail()) {
            SaveLogLevel(kDefaultLogLevel);
            return kDefaultLogLevel;
        }

        level = ClampLogLevel(level);

        // Rewrite if invalid/out of range
        SaveLogLevel(level);

        return level;
    }

    void SaveLogLevel(int level)
    {
        level = ClampLogLevel(level);

        std::error_code ec;
        std::filesystem::create_directories(kConfigDir, ec);

        std::ofstream file(kConfigPath, std::ios::trunc);
        if (!file.is_open()) {
            return;
        }

        file << level;
    }
}