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

    FileEntry() = default;
    FileEntry(const std::string& path, std::time_t ctime, std::time_t mtime,
              uint64_t sz, const std::string& desc, int64_t startBlock = -1);
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

class FileTable {
public:
    explicit FileTable(const std::string& dbPath);
    ~FileTable();

    FileTable(const FileTable&) = delete;
    FileTable& operator=(const FileTable&) = delete;
    FileTable(FileTable&& other) noexcept;
    FileTable& operator=(FileTable&& other) noexcept;

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

private:
    sqlite3* db_ = nullptr;

    void initSchema();
    bool execute(const std::string& sql) const;
    bool prepareStatement(const std::string& sql, sqlite3_stmt** stmt) const;
    void bindFileEntry(sqlite3_stmt* stmt, const FileEntry& file) const;
    FileEntry extractFileEntry(sqlite3_stmt* stmt) const;
    BlockEntry extractBlockEntry(sqlite3_stmt* stmt) const;
};

} // namespace enostorg
