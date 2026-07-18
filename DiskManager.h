#pragma once

#include <string>
#include <vector>
#include <random>

namespace enostorg {

struct DiskInfo {
    std::string name;          // 文件夹名 (e.g. "disk01")
    std::string label;         // 可读标签
    uint64_t capacity = 0;     // 总容量（字节）
    int speedRating = 5;       // 速率等级 1-10
    double weight = 0;         // 分配权重（<=0=自动计算，0=只读不可写）
    int64_t available = 0;     // 可用空间（动态跟踪）
    bool writable = true;      // weight <= 0 || capacity == 0 → 只读
};

class DiskManager {
public:
    // 扫描 disksDir 下所有子文件夹，读 diskinfo.ini
    bool discover(const std::string& disksDir);

    int diskCount() const { return static_cast<int>(disks_.size()); }
    const DiskInfo& getDisk(int index) const { return disks_[index]; }

    // 选择 N 个不同磁盘（排除列表中已选索引），返回磁盘索引
    // 不足 N 个可用磁盘时返回空
    std::vector<int> selectDisks(int count, const std::vector<int>& exclude = {}) const;

    // 追踪空间
    void trackAllocation(int diskIndex, int64_t size);
    void trackDeallocation(int diskIndex, int64_t size);
    int64_t getAvailable(int diskIndex) const { return disks_[diskIndex].available; }

    // 调试/日志
    void logStatus() const;

private:
    struct DiskSelection {
        int index;
        double weight;
    };

    std::vector<DiskInfo> disks_;
    mutable std::mt19937_64 rng_{std::random_device{}()};
};

} // namespace enostorg
