#include "FileTable.h"

#include <sqlite3.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace enostorg {

// ============================================================================
// FileEntry & BlockEntry constructors
// ============================================================================

FileEntry::FileEntry(const std::string& path, std::time_t ctime, std::time_t mtime,
                     uint64_t sz, const std::string& desc, int64_t startBlock)
    : filePath(path), createTime(ctime), modifyTime(mtime), size(sz),
      description(desc), startBlockId(startBlock) {}

BlockEntry::BlockEntry(const std::string& path, int64_t next, int64_t spare,
                       bool bad, uint64_t sz, const std::string& hash)
    : id(0), blockPath(path), nextBlockId(next), spareBlockId(spare),
      isBadBlock(bad), blockSize(sz), sha256(hash) {}

// ============================================================================
// FileTable Implementation
// ============================================================================

FileTable::FileTable(const std::string& dbPath) {
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("Cannot open database: ") + sqlite3_errmsg(db_));
    }
    initSchema();
}

FileTable::~FileTable() {
    if (db_) {
        sqlite3_close(db_);
    }
}

FileTable::FileTable(FileTable&& other) noexcept : db_(other.db_) {
    other.db_ = nullptr;
}

FileTable& FileTable::operator=(FileTable&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

// ============================================================================
// Schema Initialization
// ============================================================================

void FileTable::initSchema() {
    const char* createFilesTable = R"(
        CREATE TABLE IF NOT EXISTS files (
            file_path    TEXT PRIMARY KEY NOT NULL,
            create_time  INTEGER NOT NULL,
            modify_time  INTEGER NOT NULL,
            size         INTEGER NOT NULL,
            description  TEXT,
            start_block  INTEGER
        )
    )";

    const char* createBlocksTable = R"(
        CREATE TABLE IF NOT EXISTS blocks (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            block_path    TEXT NOT NULL,
            next_block    INTEGER,
            spare_block   INTEGER,
            is_bad        INTEGER NOT NULL DEFAULT 0,
            block_size    INTEGER NOT NULL,
            sha256        TEXT NOT NULL
        )
    )";

    execute(createFilesTable);
    execute(createBlocksTable);
}

bool FileTable::execute(const std::string& sql) const {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        if (errMsg) {
            sqlite3_free(errMsg);
        }
        return false;
    }
    return true;
}

bool FileTable::prepareStatement(const std::string& sql, sqlite3_stmt** stmt) const {
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt, nullptr);
    return rc == SQLITE_OK;
}

// ============================================================================
// FileEntry Helpers
// ============================================================================

void FileTable::bindFileEntry(sqlite3_stmt* stmt, const FileEntry& file) const {
    sqlite3_bind_text(stmt, 1, file.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(file.createTime));
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(file.modifyTime));
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(file.size));
    sqlite3_bind_text(stmt, 5, file.description.c_str(), -1, SQLITE_TRANSIENT);
    if (file.startBlockId >= 0) {
        sqlite3_bind_int64(stmt, 6, file.startBlockId);
    } else {
        sqlite3_bind_null(stmt, 6);
    }
}

FileEntry FileTable::extractFileEntry(sqlite3_stmt* stmt) const {
    FileEntry entry;
    entry.filePath = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    entry.createTime = static_cast<std::time_t>(sqlite3_column_int64(stmt, 1));
    entry.modifyTime = static_cast<std::time_t>(sqlite3_column_int64(stmt, 2));
    entry.size = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
    const char* desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    entry.description = desc ? desc : "";
    entry.startBlockId = sqlite3_column_type(stmt, 5) == SQLITE_NULL
                             ? -1
                             : sqlite3_column_int64(stmt, 5);
    return entry;
}

BlockEntry FileTable::extractBlockEntry(sqlite3_stmt* stmt) const {
    BlockEntry entry;
    entry.id = sqlite3_column_int64(stmt, 0);
    const char* path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    entry.blockPath = path ? path : "";
    entry.nextBlockId = sqlite3_column_type(stmt, 2) == SQLITE_NULL
                            ? -1
                            : sqlite3_column_int64(stmt, 2);
    entry.spareBlockId = sqlite3_column_type(stmt, 3) == SQLITE_NULL
                               ? -1
                               : sqlite3_column_int64(stmt, 3);
    entry.isBadBlock = sqlite3_column_int(stmt, 4) != 0;
    entry.blockSize = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
    const char* hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    entry.sha256 = hash ? hash : "";
    return entry;
}

// ============================================================================
// File Operations
// ============================================================================

bool FileTable::insertFile(const FileEntry& file) {
    const char* sql = R"(
        INSERT INTO files (file_path, create_time, modify_time, size, description, start_block)
        VALUES (?, ?, ?, ?, ?, ?)
    )";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;

    bindFileEntry(stmt, file);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool FileTable::deleteFile(const std::string& filePath) {
    // First delete all blocks in the chain
    auto file = getFile(filePath);
    if (file && file->startBlockId >= 0) {
        // Walk the chain and delete all blocks
        int64_t current = file->startBlockId;
        while (current >= 0) {
            auto block = getBlock(current);
            if (!block) break;
            int64_t next = block->nextBlockId;
            deleteBlock(current);
            current = next;
        }
    }

    const char* sql = "DELETE FROM files WHERE file_path = ?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool FileTable::updateFile(const FileEntry& file) {
    const char* sql = R"(
        UPDATE files SET create_time = ?, modify_time = ?, size = ?,
                         description = ?, start_block = ?
        WHERE file_path = ?
    )";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(file.createTime));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(file.modifyTime));
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(file.size));
    sqlite3_bind_text(stmt, 4, file.description.c_str(), -1, SQLITE_TRANSIENT);
    if (file.startBlockId >= 0) {
        sqlite3_bind_int64(stmt, 5, file.startBlockId);
    } else {
        sqlite3_bind_null(stmt, 5);
    }
    sqlite3_bind_text(stmt, 6, file.filePath.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<FileEntry> FileTable::getFile(const std::string& filePath) const {
    const char* sql = "SELECT * FROM files WHERE file_path = ?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return std::nullopt;

    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<FileEntry> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = extractFileEntry(stmt);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<FileEntry> FileTable::listFiles() const {
    std::vector<FileEntry> files;
    const char* sql = "SELECT * FROM files";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return files;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        files.push_back(extractFileEntry(stmt));
    }
    sqlite3_finalize(stmt);
    return files;
}

bool FileTable::fileExists(const std::string& filePath) const {
    return getFile(filePath).has_value();
}

// ============================================================================
// Block Operations
// ============================================================================

int64_t FileTable::insertBlock(const BlockEntry& block) {
    const char* sql = R"(
        INSERT INTO blocks (block_path, next_block, spare_block, is_bad, block_size, sha256)
        VALUES (?, ?, ?, ?, ?, ?)
    )";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return -1;

    sqlite3_bind_text(stmt, 1, block.blockPath.c_str(), -1, SQLITE_TRANSIENT);
    if (block.nextBlockId >= 0) {
        sqlite3_bind_int64(stmt, 2, block.nextBlockId);
    } else {
        sqlite3_bind_null(stmt, 2);
    }
    if (block.spareBlockId >= 0) {
        sqlite3_bind_int64(stmt, 3, block.spareBlockId);
    } else {
        sqlite3_bind_null(stmt, 3);
    }
    sqlite3_bind_int(stmt, 4, block.isBadBlock ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(block.blockSize));
    sqlite3_bind_text(stmt, 6, block.sha256.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    return sqlite3_last_insert_rowid(db_);
}

bool FileTable::deleteBlock(int64_t blockId) {
    // Update any blocks that point to this one
    const char* updateSql = "UPDATE blocks SET next_block = NULL WHERE next_block = ?";
    sqlite3_stmt* updateStmt = nullptr;
    if (prepareStatement(updateSql, &updateStmt)) {
        sqlite3_bind_int64(updateStmt, 1, blockId);
        sqlite3_step(updateStmt);
        sqlite3_finalize(updateStmt);
    }

    const char* sql = "DELETE FROM blocks WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;
    sqlite3_bind_int64(stmt, 1, blockId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool FileTable::updateBlock(const BlockEntry& block) {
    const char* sql = R"(
        UPDATE blocks SET block_path = ?, next_block = ?, spare_block = ?,
                          is_bad = ?, block_size = ?, sha256 = ?
        WHERE id = ?
    )";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;

    sqlite3_bind_text(stmt, 1, block.blockPath.c_str(), -1, SQLITE_TRANSIENT);
    if (block.nextBlockId >= 0) {
        sqlite3_bind_int64(stmt, 2, block.nextBlockId);
    } else {
        sqlite3_bind_null(stmt, 2);
    }
    if (block.spareBlockId >= 0) {
        sqlite3_bind_int64(stmt, 3, block.spareBlockId);
    } else {
        sqlite3_bind_null(stmt, 3);
    }
    sqlite3_bind_int(stmt, 4, block.isBadBlock ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(block.blockSize));
    sqlite3_bind_text(stmt, 6, block.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, block.id);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<BlockEntry> FileTable::getBlock(int64_t blockId) const {
    const char* sql = "SELECT * FROM blocks WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return std::nullopt;

    sqlite3_bind_int64(stmt, 1, blockId);
    std::optional<BlockEntry> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        result = extractBlockEntry(stmt);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<BlockEntry> FileTable::getFileBlocks(const std::string& filePath) const {
    std::vector<BlockEntry> blocks;
    auto file = getFile(filePath);
    if (!file || file->startBlockId < 0) return blocks;

    int64_t current = file->startBlockId;
    while (current >= 0) {
        auto block = getBlock(current);
        if (!block) break;
        blocks.push_back(*block);
        current = block->nextBlockId;
    }
    return blocks;
}

std::vector<BlockEntry> FileTable::getBadBlocks() const {
    std::vector<BlockEntry> blocks;
    const char* sql = "SELECT * FROM blocks WHERE is_bad = 1";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return blocks;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        blocks.push_back(extractBlockEntry(stmt));
    }
    sqlite3_finalize(stmt);
    return blocks;
}

// ============================================================================
// Chain Operations
// ============================================================================

bool FileTable::appendBlockToFile(const std::string& filePath, int64_t blockId) {
    auto file = getFile(filePath);
    if (!file) return false;

    if (file->startBlockId < 0) {
        // First block in the chain
        file->startBlockId = blockId;
        return updateFile(*file);
    }

    // Walk to the end of the chain
    int64_t current = file->startBlockId;
    while (true) {
        auto block = getBlock(current);
        if (!block) return false;
        if (block->nextBlockId < 0) {
            // Found the tail, append here
            block->nextBlockId = blockId;
            return updateBlock(*block);
        }
        current = block->nextBlockId;
    }
}

bool FileTable::setSpareBlock(int64_t blockId, int64_t spareBlockId) {
    auto block = getBlock(blockId);
    if (!block) return false;
    block->spareBlockId = spareBlockId;
    return updateBlock(*block);
}

bool FileTable::markBadBlock(int64_t blockId) {
    auto block = getBlock(blockId);
    if (!block) return false;
    block->isBadBlock = true;
    return updateBlock(*block);
}

// ============================================================================
// Transaction
// ============================================================================

void FileTable::beginTransaction() {
    execute("BEGIN TRANSACTION");
}

void FileTable::commitTransaction() {
    execute("COMMIT");
}

void FileTable::rollbackTransaction() {
    execute("ROLLBACK");
}

} // namespace enostorg
