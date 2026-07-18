#include "DiskManager.h"
#include "Config.h"

#include <trantor/utils/Logger.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <random>

namespace fs = std::filesystem;

namespace enostorg {

bool DiskManager::discover(const std::string& disksDir) {
    disks_.clear();

    fs::path base(disksDir);
    if (!fs::exists(base)) {
        fs::create_directories(base);
        return true; // empty but valid
    }

    for (auto& entry : fs::directory_iterator(base)) {
        if (!entry.is_directory()) continue;

        auto iniPath = entry.path() / "diskinfo.ini";
        if (!fs::exists(iniPath)) continue;  // no diskinfo → unavailable

        Config cfg;
        if (!cfg.load(iniPath.string())) continue;

        DiskInfo d;
        d.name = entry.path().filename().string();     // folder name
        d.label = cfg.get("disk.label", d.name);
        d.capacity = cfg.getInt64("disk.capacity", 0);
        d.speedRating = cfg.getInt("disk.speed_rating", 5);
        if (d.speedRating < 1) d.speedRating = 1;
        if (d.speedRating > 10) d.speedRating = 10;

        std::string w = cfg.get("disk.weight", "auto");
        if (w == "auto" || w.empty()) {
            // 自动 = 容量(MB) * 速率
            d.weight = (double)(d.capacity / 1048576) * d.speedRating;
        } else {
            d.weight = std::stod(w);
        }

        d.available = static_cast<int64_t>(d.capacity);
        d.writable = (d.weight > 0 && d.capacity > 0);

        // 计算已用空间：扫描文件
        for (auto& f : fs::recursive_directory_iterator(entry.path())) {
            if (f.is_regular_file() &&
                f.path().extension() == ".dat" &&
                f.path().filename() != "diskinfo.ini") {
                d.available -= static_cast<int64_t>(f.file_size());
            }
        }
        if (d.available < 0) d.available = 0;

        disks_.push_back(std::move(d));
    }

    LOG_INFO << "discovered " << disks_.size() << " disk(s)";
    logStatus();
    return !disks_.empty();
}

std::vector<int> DiskManager::selectDisks(int count, const std::vector<int>& exclude) const {
    if (count <= 0) return {};

    // 构建候选列表（可写、有空间、不在排除列表）
    std::vector<DiskSelection> candidates;
    auto& rng = const_cast<DiskManager*>(this)->rng_;

    for (int i = 0; i < (int)disks_.size(); i++) {
        if (!disks_[i].writable || disks_[i].available <= 0) continue;
        if (std::find(exclude.begin(), exclude.end(), i) != exclude.end()) continue;
        candidates.push_back({i, disks_[i].weight});
    }

    if ((int)candidates.size() < count) return {};  // 不足

    // 逐个抽取
    std::vector<int> result;
    std::vector<DiskSelection> pool = candidates;

    for (int n = 0; n < count; n++) {
        double totalWeight = 0;
        for (auto& c : pool) totalWeight += c.weight;
        if (totalWeight <= 0) return {};

        // 随机在 [0, totalWeight) 内
        std::uniform_real_distribution<double> dist(0.0, totalWeight);
        double r = dist(rng);
        double acc = 0;

        int picked = -1;
        for (size_t i = 0; i < pool.size(); i++) {
            acc += pool[i].weight;
            if (acc >= r) { picked = i; break; }
        }
        if (picked < 0) picked = (int)pool.size() - 1;

        result.push_back(pool[picked].index);
        pool.erase(pool.begin() + picked);
    }

    LOG_DEBUG << "selectDisks need=" << count << " got=" << result.size();
    return result;
}

void DiskManager::trackAllocation(int diskIndex, int64_t size) {
    if (diskIndex >= 0 && diskIndex < (int)disks_.size()) {
        disks_[diskIndex].available -= size;
    }
}

void DiskManager::trackDeallocation(int diskIndex, int64_t size) {
    if (diskIndex >= 0 && diskIndex < (int)disks_.size()) {
        disks_[diskIndex].available += size;
    }
}

void DiskManager::logStatus() const {
    for (size_t i = 0; i < disks_.size(); i++) {
        auto& d = disks_[i];
        LOG_INFO << "  [" << i << "] " << d.name
                 << " cap=" << (d.capacity / 1048576) << "MB"
                 << " avail=" << (d.available / 1024) << "KB"
                 << " speed=" << d.speedRating
                 << " weight=" << d.weight
                 << (d.writable ? " RW" : " RO");
    }
}

} // namespace enostorg
