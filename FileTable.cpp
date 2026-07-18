#include "FileTable.h"
#include "sha256.h"

#include <sqlite3.h>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace enostorg {

// ============================================================================
// Constructors
// ============================================================================

FileEntry::FileEntry(const std::string& path, std::time_t ctime, std::time_t mtime,
                     uint64_t sz, const std::string& desc, int64_t startBlock,
                     double activity)
    : filePath(path), createTime(ctime), modifyTime(mtime), size(sz),
      description(desc), startBlockId(startBlock), accessActivity(activity) {}

BlockEntry::BlockEntry(const std::string& path, int64_t next, int64_t spare,
                       bool bad, uint64_t sz, const std::string& hash)
    : id(0), blockPath(path), nextBlockId(next), spareBlockId(spare),
      isBadBlock(bad), blockSize(sz), sha256(hash) {}

// ============================================================================
// FileTable Implementation
// ============================================================================

FileTable::FileTable(const std::string& dbPath) : dataDir_("blocks") {
    int rc = sqlite3_open(dbPath.c_str(), &db_);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("Cannot open database: ") + sqlite3_errmsg(db_));
    }
    initSchema();
    // Create data directory if needed
    fs::create_directories(dataDir_);
}

FileTable::~FileTable() {
    if (db_) sqlite3_close(db_);
}

FileTable::FileTable(FileTable&& other) noexcept
    : db_(other.db_), chunkCfg_(other.chunkCfg_), dataDir_(std::move(other.dataDir_)) {
    other.db_ = nullptr;
}

FileTable& FileTable::operator=(FileTable&& other) noexcept {
    if (this != &other) {
        if (db_) sqlite3_close(db_);
        db_ = other.db_;
        chunkCfg_ = other.chunkCfg_;
        dataDir_ = std::move(other.dataDir_);
        other.db_ = nullptr;
    }
    return *this;
}

void FileTable::setBusyTimeout(int ms) {
    if (db_) sqlite3_busy_timeout(db_, ms);
}

// ============================================================================
// Schema
// ============================================================================

void FileTable::initSchema() {
    execute("PRAGMA foreign_keys = ON");

    execute(R"(
        CREATE TABLE IF NOT EXISTS files (
            file_path        TEXT PRIMARY KEY NOT NULL,
            create_time      DATETIME NOT NULL,
            modify_time      DATETIME NOT NULL,
            size             BIGINT NOT NULL,
            description      TEXT,
            start_block      BIGINT,
            access_activity  REAL NOT NULL DEFAULT 0.0,
            FOREIGN KEY (start_block) REFERENCES blocks(id)
        )
    )");

    execute(R"(
        CREATE TABLE IF NOT EXISTS blocks (
            id            INTEGER PRIMARY KEY AUTOINCREMENT,
            block_path    TEXT NOT NULL,
            next_block    BIGINT,
            spare_block   BIGINT,
            is_bad        BOOLEAN NOT NULL DEFAULT 0,
            block_size    BIGINT NOT NULL,
            sha256        TEXT NOT NULL
        )
    )");
}

bool FileTable::execute(const std::string& sql) const {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return rc == SQLITE_OK;
}

bool FileTable::prepareStatement(const std::string& sql, sqlite3_stmt** stmt) const {
    return sqlite3_prepare_v2(db_, sql.c_str(), -1, stmt, nullptr) == SQLITE_OK;
}

// ============================================================================
// Bind / Extract helpers
// ============================================================================

void FileTable::bindFileEntry(sqlite3_stmt* stmt, const FileEntry& file) const {
    sqlite3_bind_text(stmt, 1, file.filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(file.createTime));
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(file.modifyTime));
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(file.size));
    sqlite3_bind_text(stmt, 5, file.description.c_str(), -1, SQLITE_TRANSIENT);
    if (file.startBlockId >= 0)
        sqlite3_bind_int64(stmt, 6, file.startBlockId);
    else
        sqlite3_bind_null(stmt, 6);
    sqlite3_bind_double(stmt, 7, file.accessActivity);
}

FileEntry FileTable::extractFileEntry(sqlite3_stmt* stmt) const {
    FileEntry e;
    e.filePath      = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    e.createTime    = static_cast<std::time_t>(sqlite3_column_int64(stmt, 1));
    e.modifyTime    = static_cast<std::time_t>(sqlite3_column_int64(stmt, 2));
    e.size          = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
    const char* d   = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    e.description   = d ? d : "";
    e.startBlockId  = sqlite3_column_type(stmt, 5) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 5);
    e.accessActivity = sqlite3_column_double(stmt, 6);
    return e;
}

BlockEntry FileTable::extractBlockEntry(sqlite3_stmt* stmt) const {
    BlockEntry e;
    e.id           = sqlite3_column_int64(stmt, 0);
    const char* p  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    e.blockPath    = p ? p : "";
    e.nextBlockId  = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
    e.spareBlockId = sqlite3_column_type(stmt, 3) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 3);
    e.isBadBlock   = sqlite3_column_int(stmt, 4) != 0;
    e.blockSize    = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5));
    const char* h  = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    e.sha256       = h ? h : "";
    return e;
}

// ============================================================================
// File metadata operations
// ============================================================================

bool FileTable::insertFile(const FileEntry& file) {
    const char* sql = R"(
        INSERT INTO files(file_path,create_time,modify_time,size,description,start_block,access_activity)
        VALUES(?,?,?,?,?,?,?))";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;
    bindFileEntry(stmt, file);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool FileTable::deleteFile(const std::string& filePath) {
    auto file = getFile(filePath);
    if (file && file->startBlockId >= 0)
        deleteAllBlocks(file->startBlockId);

    const char* sql = "DELETE FROM files WHERE file_path=?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool FileTable::updateFile(const FileEntry& file) {
    const char* sql = R"(
        UPDATE files SET create_time=?,modify_time=?,size=?,description=?,start_block=?,access_activity=?
        WHERE file_path=?)";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(file.createTime));
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(file.modifyTime));
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(file.size));
    sqlite3_bind_text(stmt, 4, file.description.c_str(), -1, SQLITE_TRANSIENT);
    if (file.startBlockId >= 0)
        sqlite3_bind_int64(stmt, 5, file.startBlockId);
    else
        sqlite3_bind_null(stmt, 5);
    sqlite3_bind_double(stmt, 6, file.accessActivity);
    sqlite3_bind_text(stmt, 7, file.filePath.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<FileEntry> FileTable::getFile(const std::string& filePath) const {
    const char* sql = "SELECT*FROM files WHERE file_path=?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return std::nullopt;
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<FileEntry> r;
    if (sqlite3_step(stmt) == SQLITE_ROW) r = extractFileEntry(stmt);
    sqlite3_finalize(stmt);
    return r;
}

std::vector<FileEntry> FileTable::listFiles() const {
    std::vector<FileEntry> files;
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement("SELECT*FROM files", &stmt)) return files;
    while (sqlite3_step(stmt) == SQLITE_ROW) files.push_back(extractFileEntry(stmt));
    sqlite3_finalize(stmt);
    return files;
}

bool FileTable::fileExists(const std::string& filePath) const {
    return getFile(filePath).has_value();
}

// ============================================================================
// Block metadata operations
// ============================================================================

int64_t FileTable::insertBlock(const BlockEntry& block) {
    const char* sql = R"(
        INSERT INTO blocks(block_path,next_block,spare_block,is_bad,block_size,sha256)
        VALUES(?,?,?,?,?,?))";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return -1;
    sqlite3_bind_text(stmt, 1, block.blockPath.c_str(), -1, SQLITE_TRANSIENT);
    if (block.nextBlockId >= 0)
        sqlite3_bind_int64(stmt, 2, block.nextBlockId);
    else
        sqlite3_bind_null(stmt, 2);
    if (block.spareBlockId >= 0)
        sqlite3_bind_int64(stmt, 3, block.spareBlockId);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int(stmt, 4, block.isBadBlock ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(block.blockSize));
    sqlite3_bind_text(stmt, 6, block.sha256.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db_) : -1;
}

bool FileTable::deleteBlock(int64_t blockId) {
    auto block = getBlock(blockId);
    if (!block) return false;

    // 删除块文件
    deleteBlockFile(block->blockPath);

    // 解除前驱的 next_block 引用
    const char* unlinkSql = "UPDATE blocks SET next_block=NULL WHERE next_block=?";
    sqlite3_stmt* us = nullptr;
    if (prepareStatement(unlinkSql, &us)) {
        sqlite3_bind_int64(us, 1, blockId);
        sqlite3_step(us);
        sqlite3_finalize(us);
    }

    const char* sql = "DELETE FROM blocks WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;
    sqlite3_bind_int64(stmt, 1, blockId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool FileTable::updateBlock(const BlockEntry& block) {
    const char* sql = R"(
        UPDATE blocks SET block_path=?,next_block=?,spare_block=?,is_bad=?,block_size=?,sha256=?
        WHERE id=?)";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;
    sqlite3_bind_text(stmt, 1, block.blockPath.c_str(), -1, SQLITE_TRANSIENT);
    if (block.nextBlockId >= 0)
        sqlite3_bind_int64(stmt, 2, block.nextBlockId);
    else
        sqlite3_bind_null(stmt, 2);
    if (block.spareBlockId >= 0)
        sqlite3_bind_int64(stmt, 3, block.spareBlockId);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_int(stmt, 4, block.isBadBlock ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(block.blockSize));
    sqlite3_bind_text(stmt, 6, block.sha256.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, block.id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

std::optional<BlockEntry> FileTable::getBlock(int64_t blockId) const {
    const char* sql = "SELECT*FROM blocks WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return std::nullopt;
    sqlite3_bind_int64(stmt, 1, blockId);
    std::optional<BlockEntry> r;
    if (sqlite3_step(stmt) == SQLITE_ROW) r = extractBlockEntry(stmt);
    sqlite3_finalize(stmt);
    return r;
}

std::vector<BlockEntry> FileTable::getFileBlocks(const std::string& filePath) const {
    std::vector<BlockEntry> blocks;
    auto file = getFile(filePath);
    if (!file || file->startBlockId < 0) return blocks;
    int64_t cur = file->startBlockId;
    while (cur >= 0) {
        auto b = getBlock(cur);
        if (!b) break;
        blocks.push_back(*b);
        cur = b->nextBlockId;
    }
    return blocks;
}

std::vector<BlockEntry> FileTable::getBadBlocks() const {
    std::vector<BlockEntry> blocks;
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement("SELECT*FROM blocks WHERE is_bad=1", &stmt)) return blocks;
    while (sqlite3_step(stmt) == SQLITE_ROW) blocks.push_back(extractBlockEntry(stmt));
    sqlite3_finalize(stmt);
    return blocks;
}

// ============================================================================
// Chain
// ============================================================================

bool FileTable::appendBlockToFile(const std::string& filePath, int64_t blockId) {
    auto file = getFile(filePath);
    if (!file) return false;
    if (file->startBlockId < 0) {
        file->startBlockId = blockId;
        return updateFile(*file);
    }
    int64_t cur = file->startBlockId;
    while (true) {
        auto b = getBlock(cur);
        if (!b) return false;
        if (b->nextBlockId < 0) {
            b->nextBlockId = blockId;
            return updateBlock(*b);
        }
        cur = b->nextBlockId;
    }
}

bool FileTable::setSpareBlock(int64_t blockId, int64_t spareBlockId) {
    auto b = getBlock(blockId);
    if (!b) return false;
    b->spareBlockId = spareBlockId;
    return updateBlock(*b);
}

bool FileTable::markBadBlock(int64_t blockId) {
    auto b = getBlock(blockId);
    if (!b) return false;
    b->isBadBlock = true;
    return updateBlock(*b);
}

// ============================================================================
// Transaction
// ============================================================================

void FileTable::beginTransaction() { execute("BEGIN TRANSACTION"); }
void FileTable::commitTransaction() { execute("COMMIT"); }
void FileTable::rollbackTransaction() { execute("ROLLBACK"); }

// ============================================================================
// Filesystem helpers
// ============================================================================

std::string FileTable::resolveBlockPath(const std::string& blockPath) const {
    if (blockPath.empty()) return "";
    if (fs::path(blockPath).is_absolute()) return blockPath;
    return (fs::path(dataDir_) / blockPath).string();
}

std::string FileTable::generateBlockPath() const {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()).count();
    int n = ++blockCounter_;
    std::ostringstream oss;
    oss << "block_" << ms << "_" << n << ".dat";
    return oss.str();
}

std::vector<uint8_t> FileTable::readBlockFile(const std::string& blockPath) const {
    std::string full = resolveBlockPath(blockPath);
    std::ifstream f(full, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

bool FileTable::writeBlockFile(const std::string& blockPath, const std::vector<uint8_t>& data) const {
    std::string full = resolveBlockPath(blockPath);
    fs::create_directories(fs::path(full).parent_path());
    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return f.good();
}

bool FileTable::deleteBlockFile(const std::string& blockPath) const {
    std::string full = resolveBlockPath(blockPath);
    std::error_code ec;
    return fs::remove(full, ec);
}

void FileTable::deleteAllBlocks(int64_t startBlockId) {
    int64_t cur = startBlockId;
    while (cur >= 0) {
        auto b = getBlock(cur);
        if (!b) break;
        int64_t next = b->nextBlockId;
        deleteBlockFile(b->blockPath);
        // Delete metadata too
        sqlite3_stmt* stmt = nullptr;
        if (prepareStatement("DELETE FROM blocks WHERE id=?", &stmt)) {
            sqlite3_bind_int64(stmt, 1, cur);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        cur = next;
    }
}

std::vector<uint8_t> FileTable::readBlocksData(const std::string& filePath) const {
    auto blocks = getFileBlocks(filePath);
    std::vector<uint8_t> result;
    for (auto& b : blocks) {
        auto data = readBlockFile(b.blockPath);
        result.insert(result.end(), data.begin(), data.end());
    }
    return result;
}

// ============================================================================
// Chunking
// ============================================================================

std::vector<std::vector<uint8_t>> FileTable::chunkData(const std::vector<uint8_t>& data) const {
    if (chunkCfg_.strategy == "fixed")
        return chunkFixed(data);
    return chunkVariable(data);
}

std::vector<std::vector<uint8_t>> FileTable::chunkFixed(const std::vector<uint8_t>& data) const {
    std::vector<std::vector<uint8_t>> chunks;
    uint64_t sz = chunkCfg_.fixedSize;
    if (sz == 0) sz = 262144;
    size_t off = 0;
    while (off < data.size()) {
        size_t len = (std::min)(static_cast<size_t>(sz), data.size() - off);
        chunks.emplace_back(data.begin() + off, data.begin() + off + len);
        off += len;
    }
    return chunks;
}

std::vector<std::vector<uint8_t>> FileTable::chunkVariable(const std::vector<uint8_t>& data) const {
    std::vector<std::vector<uint8_t>> chunks;
    size_t w = chunkCfg_.rollingHashWindow;
    uint64_t minSz = chunkCfg_.minChunkSize;
    uint64_t maxSz = chunkCfg_.maxChunkSize;
    int maskBits = chunkCfg_.rollingHashMaskBits;
    if (w == 0) w = 48;
    if (minSz == 0) minSz = 65536;
    if (maxSz == 0) maxSz = minSz * 16;
    if (maskBits == 0) maskBits = 12;

    const uint32_t BASE = 256;
    uint32_t powW = 1;
    for (size_t i = 0; i < w - 1; i++) powW *= BASE;

    uint32_t mask = (1u << maskBits) - 1;
    uint32_t hash = 0;
    size_t chunkStart = 0;

    for (size_t i = 0; i < data.size(); i++) {
        if (i >= w)
            hash = ((hash - (uint32_t)data[i - w] * powW) * BASE) + data[i];
        else
            hash = hash * BASE + data[i];

        uint64_t chunkLen = i - chunkStart + 1;
        if (chunkLen >= maxSz || (chunkLen >= minSz && (hash & mask) == 0) || i == data.size() - 1) {
            chunks.emplace_back(data.begin() + chunkStart, data.begin() + i + 1);
            chunkStart = i + 1;
            hash = 0;
        }
    }
    if (chunkStart < data.size())
        chunks.emplace_back(data.begin() + chunkStart, data.end());
    return chunks;
}

uint32_t FileTable::rollingHash(const uint8_t* data, size_t len) const {
    uint32_t h = 0;
    for (size_t i = 0; i < len; i++) h = h * 256 + data[i];
    return h;
}

// ============================================================================
// Object-level operations (filesystem storage)
// ============================================================================

std::optional<FileEntry> FileTable::storeObject(const std::string& filePath,
                                                 const std::vector<uint8_t>& data,
                                                 const std::string& description) {
    if (fileExists(filePath)) return std::nullopt;

    auto chunks = chunkData(data);
    beginTransaction();

    int64_t firstId = -1, prevId = -1;

    for (auto& chunk : chunks) {
        std::string bpath = generateBlockPath();
        std::string sha = SHA256::hash(chunk);
        uint64_t sz = chunk.size();

        // Write file
        if (!writeBlockFile(bpath, chunk)) {
            rollbackTransaction();
            return std::nullopt;
        }

        BlockEntry block;
        block.blockPath = bpath;
        block.blockSize = sz;
        block.sha256 = sha;
        block.nextBlockId = -1;
        block.spareBlockId = -1;
        block.isBadBlock = false;

        int64_t newId = insertBlock(block);
        if (newId < 0) { rollbackTransaction(); return std::nullopt; }

        if (firstId < 0) firstId = newId;
        if (prevId >= 0) {
            auto prev = getBlock(prevId);
            if (prev) { prev->nextBlockId = newId; updateBlock(*prev); }
        }
        prevId = newId;
    }

    FileEntry f;
    f.filePath = filePath;
    f.size = data.size();
    auto now = std::time(nullptr);
    f.createTime = now;
    f.modifyTime = now;
    f.description = description;
    f.startBlockId = firstId;
    f.accessActivity = 0.0;

    if (!insertFile(f)) { rollbackTransaction(); return std::nullopt; }
    commitTransaction();
    return f;
}

std::vector<uint8_t> FileTable::getObjectData(const std::string& filePath) const {
    return readBlocksData(filePath);
}

std::optional<FileTable::RangeResult> FileTable::getObjectDataRange(
    const std::string& filePath, uint64_t start, uint64_t end) const {

    auto file = getFile(filePath);
    if (!file) return std::nullopt;
    if (file->size == 0) {
        RangeResult rr; rr.totalSize = 0; rr.rangeStart = 0; rr.rangeEnd = 0;
        return rr;
    }
    if (end >= file->size) end = file->size - 1;
    if (start > end) start = end;

    uint64_t reqLen = end - start + 1;
    std::vector<uint8_t> result;
    result.reserve(static_cast<size_t>(reqLen));
    uint64_t cumOff = 0;

    int64_t curId = file->startBlockId;
    while (curId >= 0 && cumOff <= end) {
        auto block = getBlock(curId);
        if (!block) break;

        uint64_t bsz = block->blockSize;
        uint64_t bStart = cumOff;
        uint64_t bEnd = cumOff + bsz - 1;

        if (bEnd >= start && bStart <= end) {
            auto filedata = readBlockFile(block->blockPath);
            uint64_t copyStart = (std::max)(start, bStart) - bStart;
            uint64_t copyEnd   = (std::min)(end, bEnd) - bStart;
            uint64_t copyLen   = copyEnd - copyStart + 1;
            if (copyStart < filedata.size() && copyStart + copyLen <= filedata.size()) {
                result.insert(result.end(),
                              filedata.begin() + copyStart,
                              filedata.begin() + copyStart + copyLen);
            }
        }
        cumOff += bsz;
        curId = block->nextBlockId;
    }

    RangeResult rr;
    rr.data = std::move(result);
    rr.totalSize = file->size;
    rr.rangeStart = start;
    rr.rangeEnd = end;
    return rr;
}

std::optional<FileEntry> FileTable::appendObjectData(const std::string& filePath,
                                                      const std::vector<uint8_t>& data) {
    auto file = getFile(filePath);
    if (!file) return std::nullopt;
    if (data.empty()) return file;

    auto chunks = chunkData(data);
    beginTransaction();

    // Find tail
    int64_t tailId = -1;
    if (file->startBlockId >= 0) {
        int64_t cur = file->startBlockId;
        while (true) {
            auto b = getBlock(cur);
            if (!b) { rollbackTransaction(); return std::nullopt; }
            if (b->nextBlockId < 0) { tailId = cur; break; }
            cur = b->nextBlockId;
        }
    }

    int64_t prevId = tailId;
    for (auto& chunk : chunks) {
        std::string bpath = generateBlockPath();
        std::string sha = SHA256::hash(chunk);
        if (!writeBlockFile(bpath, chunk)) { rollbackTransaction(); return std::nullopt; }

        BlockEntry block;
        block.blockPath = bpath;
        block.blockSize = chunk.size();
        block.sha256 = sha;
        block.nextBlockId = -1;
        block.spareBlockId = -1;
        block.isBadBlock = false;

        int64_t newId = insertBlock(block);
        if (newId < 0) { rollbackTransaction(); return std::nullopt; }

        if (prevId < 0) file->startBlockId = newId;
        else {
            auto prev = getBlock(prevId);
            if (prev) { prev->nextBlockId = newId; updateBlock(*prev); }
        }
        prevId = newId;
    }

    file->size += data.size();
    file->modifyTime = std::time(nullptr);
    updateFile(*file);
    commitTransaction();
    return file;
}

std::optional<FileEntry> FileTable::patchObjectData(const std::string& filePath,
                                                     uint64_t offset,
                                                     const std::vector<uint8_t>& data) {
    auto file = getFile(filePath);
    if (!file) return std::nullopt;
    if (data.empty()) return file;

    // Read all, modify in memory, re-chunk, rewrite all block files
    auto fullData = readBlocksData(filePath);
    if (fullData.size() < offset) return std::nullopt;

    if (offset + data.size() > fullData.size())
        fullData.resize(offset + data.size());
    std::memcpy(fullData.data() + offset, data.data(), data.size());

    // Delete old block files and metadata
    deleteAllBlocks(file->startBlockId);

    // Re-chunk
    auto chunks = chunkData(fullData);
    beginTransaction();

    int64_t firstId = -1, prevId = -1;
    for (auto& chunk : chunks) {
        std::string bpath = generateBlockPath();
        std::string sha = SHA256::hash(chunk);
        if (!writeBlockFile(bpath, chunk)) { rollbackTransaction(); return std::nullopt; }

        BlockEntry block;
        block.blockPath = bpath;
        block.blockSize = chunk.size();
        block.sha256 = sha;
        block.nextBlockId = -1;
        block.spareBlockId = -1;
        block.isBadBlock = false;

        int64_t newId = insertBlock(block);
        if (newId < 0) { rollbackTransaction(); return std::nullopt; }

        if (firstId < 0) firstId = newId;
        if (prevId >= 0) {
            auto prev = getBlock(prevId);
            if (prev) { prev->nextBlockId = newId; updateBlock(*prev); }
        }
        prevId = newId;
    }

    file->startBlockId = firstId;
    file->size = fullData.size();
    file->modifyTime = std::time(nullptr);
    updateFile(*file);
    commitTransaction();
    return file;
}

bool FileTable::renameObject(const std::string& oldPath, const std::string& newPath) {
    if (!fileExists(oldPath) || fileExists(newPath)) return false;
    const char* sql = "UPDATE files SET file_path=?,modify_time=? WHERE file_path=?";
    sqlite3_stmt* stmt = nullptr;
    if (!prepareStatement(sql, &stmt)) return false;
    sqlite3_bind_text(stmt, 1, newPath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(std::time(nullptr)));
    sqlite3_bind_text(stmt, 3, oldPath.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool FileTable::deleteObject(const std::string& filePath) {
    return deleteFile(filePath);
}

} // namespace enostorg
