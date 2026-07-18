#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <ctime>

struct sqlite3;
struct sqlite3_stmt;

namespace enostorg {

struct FileEntry {
    std::string filePath;
    std::time_t createTime;
    std::time_t modifyTime;
    uint64_t size;
    std::string description;
    int64_t startBlockId;
    double accessActivity;

    FileEntry() : accessActivity(0.0) {}
    FileEntry(const std::string& path, std::time_t ctime, std::time_t mtime,
              uint64_t sz, const std::string& desc, int64_t startBlock = -1,
              double activity = 0.0);
};

struct BlockEntry {
    int64_t id;
    std::string blockPath;
    int64_t nextBlockId;
    int64_t spareBlockId;
    bool isBadBlock;
    uint64_t blockSize;
    std::string sha256;

    BlockEntry() = default;
    BlockEntry(const std::string& path, int64_t next, int64_t spare,
               bool bad, uint64_t sz, const std::string& hash);
};

struct ChunkConfig {
    std::string strategy = "variable";
    uint64_t fixedSize = 262144;
    uint64_t minChunkSize = 65536;
    uint64_t maxChunkSize = 1048576;
    size_t rollingHashWindow = 48;
    int rollingHashMaskBits = 12;
};

class FileTable {
public:
    explicit FileTable(const std::string& dbPath);
    ~FileTable();

    FileTable(const FileTable&) = delete;
    FileTable& operator=(const FileTable&) = delete;
    FileTable(FileTable&& other) noexcept;
    FileTable& operator=(FileTable&& other) noexcept;

    void setBusyTimeout(int ms);
    void setDataDir(const std::string& dir) { dataDir_ = dir; }

    // ---- 元数据接口 ----
    bool insertFile(const FileEntry& file);
    bool deleteFile(const std::string& filePath);
    bool updateFile(const FileEntry& file);
    std::optional<FileEntry> getFile(const std::string& filePath) const;
    std::vector<FileEntry> listFiles() const;
    bool fileExists(const std::string& filePath) const;

    int64_t insertBlock(const BlockEntry& block);
    bool deleteBlock(int64_t blockId);
    bool updateBlock(const BlockEntry& block);
    std::optional<BlockEntry> getBlock(int64_t blockId) const;
    std::vector<BlockEntry> getFileBlocks(const std::string& filePath) const;
    std::vector<BlockEntry> getBadBlocks() const;

    bool appendBlockToFile(const std::string& filePath, int64_t blockId);
    bool setSpareBlock(int64_t blockId, int64_t spareBlockId);
    bool markBadBlock(int64_t blockId);

    void beginTransaction();
    void commitTransaction();
    void rollbackTransaction();

    // ---- 分块配置 ----
    void setChunkConfig(const ChunkConfig& cfg) { chunkCfg_ = cfg; }
    const ChunkConfig& chunkConfig() const { return chunkCfg_; }

    // ---- 基于文件系统的对象操作（数据存文件，block_path 记录路径） ----
    // 读取单个块文件的数据
    std::vector<uint8_t> readBlockFile(const std::string& blockPath) const;
    // 写入单个块文件
    bool writeBlockFile(const std::string& blockPath, const std::vector<uint8_t>& data) const;
    // 删除单个块文件
    bool deleteBlockFile(const std::string& blockPath) const;
    // 读取链上所有块文件，返回完整数据
    std::vector<uint8_t> readBlocksData(const std::string& filePath) const;

    // 存储对象：分块 → 写块文件 → 创建元数据记录
    std::optional<FileEntry> storeObject(const std::string& filePath,
                                         const std::vector<uint8_t>& data,
                                         const std::string& description = "");

    // 获取完整对象数据
    std::vector<uint8_t> getObjectData(const std::string& filePath) const;

    // 带 Range 的部分读取
    struct RangeResult {
        std::vector<uint8_t> data;
        uint64_t totalSize;
        uint64_t rangeStart;
        uint64_t rangeEnd;
    };
    std::optional<RangeResult> getObjectDataRange(const std::string& filePath,
                                                  uint64_t start,
                                                  uint64_t end) const;

    // 追加数据
    std::optional<FileEntry> appendObjectData(const std::string& filePath,
                                              const std::vector<uint8_t>& data);

    // PATCH 写入（指定偏移处）
    std::optional<FileEntry> patchObjectData(const std::string& filePath,
                                             uint64_t offset,
                                             const std::vector<uint8_t>& data);

    // 重命名
    bool renameObject(const std::string& oldPath, const std::string& newPath);

    // 删除对象（元数据 + 所有块文件）
    bool deleteObject(const std::string& filePath);

private:
    sqlite3* db_ = nullptr;
    ChunkConfig chunkCfg_;
    std::string dataDir_;
    mutable int blockCounter_ = 0;

    void initSchema();
    bool execute(const std::string& sql) const;
    bool prepareStatement(const std::string& sql, sqlite3_stmt** stmt) const;
    void bindFileEntry(sqlite3_stmt* stmt, const FileEntry& file) const;
    FileEntry extractFileEntry(sqlite3_stmt* stmt) const;
    BlockEntry extractBlockEntry(sqlite3_stmt* stmt) const;

    std::string resolveBlockPath(const std::string& blockPath) const;
    std::string generateBlockPath() const;
    void deleteAllBlocks(int64_t startBlockId);

    // 分块
    std::vector<std::vector<uint8_t>> chunkData(const std::vector<uint8_t>& data) const;
    std::vector<std::vector<uint8_t>> chunkFixed(const std::vector<uint8_t>& data) const;
    std::vector<std::vector<uint8_t>> chunkVariable(const std::vector<uint8_t>& data) const;
    uint32_t rollingHash(const uint8_t* data, size_t len) const;
};

} // namespace enostorg
