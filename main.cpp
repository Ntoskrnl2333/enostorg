#include "FileTable.h"
#include "DiskManager.h"
#include "Config.h"
#include <drogon/drogon.h>
#include <trantor/utils/AsyncFileLogger.h>
#include <json/value.h>
#include <json/writer.h>
#include <sstream>
#include <regex>
#include <filesystem>

namespace fs = std::filesystem;

using namespace enostorg;
using namespace drogon;

namespace {

Json::Value fileToJson(const FileEntry& f) {
    Json::Value j;
    j["file_path"]     = f.filePath;
    j["create_time"]   = static_cast<Json::Int64>(f.createTime);
    j["modify_time"]   = static_cast<Json::Int64>(f.modifyTime);
    j["size"]          = static_cast<Json::Int64>(f.size);
    j["description"]   = f.description;
    j["start_block"]   = f.startBlockId >= 0
                           ? Json::Value(static_cast<Json::Int64>(f.startBlockId))
                           : Json::Value(Json::nullValue);
    j["access_activity"] = f.accessActivity;
    return j;
}

Json::Value blockToJson(const BlockEntry& b) {
    Json::Value j;
    j["id"]          = static_cast<Json::Int64>(b.id);
    j["block_path"]  = b.blockPath;
    j["next_block"]  = b.nextBlockId >= 0
                         ? Json::Value(static_cast<Json::Int64>(b.nextBlockId))
                         : Json::Value(Json::nullValue);
    j["spare_block"] = b.spareBlockId >= 0
                         ? Json::Value(static_cast<Json::Int64>(b.spareBlockId))
                         : Json::Value(Json::nullValue);
    j["is_bad"]      = b.isBadBlock;
    j["block_size"]  = static_cast<Json::Int64>(b.blockSize);
    j["sha256"]      = b.sha256;
    return j;
}

HttpResponsePtr err(int code, const std::string& msg) {
    Json::Value j; j["error"] = msg;
    auto r = HttpResponse::newHttpJsonResponse(j);
    r->setStatusCode(static_cast<HttpStatusCode>(code));
    return r;
}

HttpResponsePtr ok() {
    Json::Value j; j["status"] = "ok";
    return HttpResponse::newHttpJsonResponse(j);
}

std::string getQueryParam(const HttpRequestPtr& req, const std::string& key) {
    auto& params = req->getParameters();
    auto it = params.find(key);
    return it != params.end() ? it->second : "";
}

bool parseRange(const std::string& rangeHeader, uint64_t& start, uint64_t& end,
                bool& hasEnd) {
    std::regex re(R"(bytes\s*=\s*(\d+)\s*-\s*(\d*))", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(rangeHeader, m, re)) return false;
    start = std::stoull(m[1].str());
    hasEnd = m[2].matched;
    if (hasEnd) end = std::stoull(m[2].str());
    return true;
}

} // anonymous namespace

static std::shared_ptr<trantor::AsyncFileLogger> g_fileLogger;

int main()
{
    // ---- 加载配置 ----
    Config cfg;
    if (!cfg.load("config.ini"))
        LOG_WARN << "config.ini not found, using defaults";

    std::string dbPath      = cfg.get("database.path", "storage.db");
    int busyTimeout         = cfg.getInt("database.busy_timeout_ms", 5000);
    std::string logLevel    = cfg.get("logging.level", "info");
    std::string logFile     = cfg.get("logging.file", "");
    bool logConsole         = cfg.getBool("logging.console", true);
    std::string listenAddr  = cfg.get("server.listen", "0.0.0.0");
    int listenPort          = cfg.getInt("server.port", 8080);
    std::string blocksDir   = cfg.get("storage.blocks_dir", "blocks");

    // ---- 日志初始化（必须在任何日志输出前） ----
    {
        trantor::Logger::LogLevel lvl = trantor::Logger::kInfo;
        if (logLevel == "trace")      lvl = trantor::Logger::kTrace;
        else if (logLevel == "debug")  lvl = trantor::Logger::kDebug;
        else if (logLevel == "warn")   lvl = trantor::Logger::kWarn;
        else if (logLevel == "error")  lvl = trantor::Logger::kError;
        else if (logLevel == "fatal")  lvl = trantor::Logger::kFatal;
        trantor::Logger::setLogLevel(lvl);
    }
    if (!logFile.empty()) {
        g_fileLogger = std::make_shared<trantor::AsyncFileLogger>();
        auto lp = fs::path(logFile);
        g_fileLogger->setFileName(lp.stem().string(), ".log",
                                  lp.parent_path().string());
        g_fileLogger->startLogging();
        trantor::Logger::setOutputFunction(
            [](const char* msg, const uint64_t len) { g_fileLogger->output(msg, len); },
            []() { g_fileLogger->flush(); });
    } else if (!logConsole) {
        trantor::Logger::setOutputFunction([](const char*, uint64_t){}, [](){});
    }

    auto ft = std::make_shared<FileTable>(dbPath);
    ft->setBusyTimeout(busyTimeout);
    ft->setDataDir(blocksDir);

    // 分块配置
    ChunkConfig chunkCfg;
    chunkCfg.strategy            = cfg.get("chunking.strategy", "variable");
    chunkCfg.fixedSize           = cfg.getInt("chunking.fixed_size", 262144);
    chunkCfg.minChunkSize        = cfg.getInt("chunking.min_chunk_size", 65536);
    chunkCfg.maxChunkSize        = cfg.getInt("chunking.max_chunk_size", 1048576);
    chunkCfg.rollingHashWindow   = cfg.getInt("chunking.rolling_hash_window", 48);
    chunkCfg.rollingHashMaskBits = cfg.getInt("chunking.rolling_hash_mask_bits", 12);
    ft->setChunkConfig(chunkCfg);

    // ---- 磁盘发现与备份配置 ----
    std::string disksDir = cfg.get("storage.blocks_dir", "blocks");
    auto dm = std::make_shared<DiskManager>();
    dm->discover(disksDir);
    ft->setDiskManager(dm.get());

    BackupConfig bkCfg;
    bkCfg.strategy = cfg.get("backup.strategy", "mirror");
    bkCfg.replicas = cfg.getInt("backup.replicas", 1);
    ft->setBackupConfig(bkCfg);

    // ---- Web UI ----
    static const char kPage[] =
#include "page.inc"
    ;
    app().registerHandler("/", [](const HttpRequestPtr&,
        std::function<void(const HttpResponsePtr&)>&& cb) {
        auto r = HttpResponse::newHttpResponse();
        r->setContentTypeCode(CT_TEXT_HTML);
        r->setBody(std::string(kPage));
        cb(r);
    });

    // ===================================================================
    // /api/files
    // ===================================================================
    app().registerHandler("/api/files",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            if (req->getMethod() == Get) {
                auto qpath = getQueryParam(req, "path");
                if (!qpath.empty()) {
                    auto f = ft->getFile(qpath);
                    if (!f) { cb(err(404, "not found")); return; }
                    cb(HttpResponse::newHttpJsonResponse(fileToJson(*f)));
                    return;
                }
                Json::Value arr(Json::arrayValue);
                for (auto& f : ft->listFiles())
                    arr.append(fileToJson(f));
                cb(HttpResponse::newHttpJsonResponse(arr));
                return;
            }
            if (req->getMethod() == Post) {
                Json::Value body;
                Json::CharReaderBuilder b;
                std::string errs;
                std::istringstream s(std::string(req->bodyData(), req->bodyLength()));
                if (!Json::parseFromStream(b, s, &body, &errs)) { cb(err(400, "bad json")); return; }
                FileEntry f;
                f.filePath     = body.get("file_path","").asString();
                f.size         = body.get("size", Json::Value(0)).asInt64();
                f.description  = body.get("description","").asString();
                f.createTime   = body.get("create_time", Json::Value(0)).asInt64();
                f.modifyTime   = body.get("modify_time", Json::Value(0)).asInt64();
                f.startBlockId = body.isMember("start_block") ? body["start_block"].asInt64() : -1;
                f.accessActivity = body.isMember("access_activity") ? body["access_activity"].asDouble() : 0.0;
                if (f.filePath.empty()) { cb(err(400, "file_path required")); return; }
                if (!ft->insertFile(f)) { cb(err(409, "exists")); return; }
                auto r = HttpResponse::newHttpJsonResponse(fileToJson(f));
                r->setStatusCode(k201Created); cb(r);
                return;
            }
            if (req->getMethod() == Put) {
                auto qpath = getQueryParam(req, "path");
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                auto f = ft->getFile(qpath);
                if (!f) { cb(err(404, "not found")); return; }
                Json::Value body;
                Json::CharReaderBuilder b;
                std::string errs;
                std::istringstream s(std::string(req->bodyData(), req->bodyLength()));
                if (!Json::parseFromStream(b, s, &body, &errs)) { cb(err(400, "bad json")); return; }
                if (body.isMember("size"))          f->size           = body["size"].asInt64();
                if (body.isMember("description"))   f->description    = body["description"].asString();
                if (body.isMember("create_time"))   f->createTime     = body["create_time"].asInt64();
                if (body.isMember("modify_time"))   f->modifyTime     = body["modify_time"].asInt64();
                if (body.isMember("start_block"))   f->startBlockId   = body["start_block"].asInt64();
                if (body.isMember("access_activity")) f->accessActivity = body["access_activity"].asDouble();
                ft->updateFile(*f);
                cb(HttpResponse::newHttpJsonResponse(fileToJson(*f)));
                return;
            }
            if (req->getMethod() == Delete) {
                auto qpath = getQueryParam(req, "path");
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                if (!ft->deleteFile(qpath)) { cb(err(404, "not found")); return; }
                cb(ok()); return;
            }
            cb(err(405, "method not allowed"));
        });

    // GET /api/files/blocks?path=...
    app().registerHandler("/api/files/blocks",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qpath = getQueryParam(req, "path");
            if (qpath.empty()) { cb(err(400, "?path required")); return; }
            Json::Value arr(Json::arrayValue);
            for (auto& b : ft->getFileBlocks(qpath))
                arr.append(blockToJson(b));
            cb(HttpResponse::newHttpJsonResponse(arr));
        }, {Get});

    // POST /api/files/blocks?path=...&block=N
    app().registerHandler("/api/files/blocks",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qpath  = getQueryParam(req, "path");
            auto qblock = getQueryParam(req, "block");
            if (qpath.empty() || qblock.empty()) { cb(err(400, "?path and ?block required")); return; }
            int64_t bid = std::stoll(qblock);
            if (!ft->appendBlockToFile(qpath, bid)) { cb(err(404, "file or block not found")); return; }
            cb(ok());
        }, {Post});

    // POST /api/blocks
    app().registerHandler("/api/blocks",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            Json::Value body;
            Json::CharReaderBuilder b;
            std::string errs;
            std::istringstream s(std::string(req->bodyData(), req->bodyLength()));
            if (!Json::parseFromStream(b, s, &body, &errs)) { cb(err(400, "bad json")); return; }
            BlockEntry bk;
            bk.blockPath   = body.get("block_path","").asString();
            bk.sha256      = body.get("sha256","").asString();
            bk.blockSize   = body.get("block_size", Json::Value(0)).asInt64();
            bk.isBadBlock  = body.get("is_bad", false).asBool();
            bk.nextBlockId = -1; bk.spareBlockId = -1;
            if (bk.blockPath.empty() || bk.sha256.empty()) { cb(err(400, "block_path, sha256, block_size required")); return; }
            int64_t id = ft->insertBlock(bk);
            if (id < 0) { cb(err(500, "insert failed")); return; }
            bk.id = id;
            auto r = HttpResponse::newHttpJsonResponse(blockToJson(bk));
            r->setStatusCode(k201Created); cb(r);
        }, {Post});

    // PUT /api/blocks/update?block=N
    app().registerHandler("/api/blocks/update",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qblock = getQueryParam(req, "block");
            if (qblock.empty()) { cb(err(400, "?block required")); return; }
            int64_t id = std::stoll(qblock);
            auto bk = ft->getBlock(id);
            if (!bk) { cb(err(404, "not found")); return; }
            Json::Value body;
            Json::CharReaderBuilder b;
            std::string errs;
            std::istringstream s(std::string(req->bodyData(), req->bodyLength()));
            if (!Json::parseFromStream(b, s, &body, &errs)) { cb(err(400, "bad json")); return; }
            if (body.isMember("block_path"))  bk->blockPath  = body["block_path"].asString();
            if (body.isMember("sha256"))      bk->sha256     = body["sha256"].asString();
            if (body.isMember("block_size"))  bk->blockSize  = body["block_size"].asInt64();
            if (body.isMember("is_bad"))      bk->isBadBlock = body["is_bad"].asBool();
            if (body.isMember("next_block"))  bk->nextBlockId = body["next_block"].asInt64();
            if (body.isMember("spare_block")) bk->spareBlockId = body["spare_block"].asInt64();
            ft->updateBlock(*bk);
            cb(HttpResponse::newHttpJsonResponse(blockToJson(*bk)));
        }, {Put});

    // DELETE /api/blocks/delete?block=N
    app().registerHandler("/api/blocks/delete",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qblock = getQueryParam(req, "block");
            if (qblock.empty()) { cb(err(400, "?block required")); return; }
            int64_t id = std::stoll(qblock);
            if (!ft->deleteBlock(id)) { cb(err(404, "not found")); return; }
            cb(ok());
        }, {Delete});

    // PATCH /api/blocks/bad?block=N
    app().registerHandler("/api/blocks/bad",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qblock = getQueryParam(req, "block");
            if (qblock.empty()) { cb(err(400, "?block required")); return; }
            int64_t id = std::stoll(qblock);
            if (!ft->markBadBlock(id)) { cb(err(404, "not found")); return; }
            cb(ok());
        }, {Patch});

    // PATCH /api/blocks/spare?block=N&spare=M
    app().registerHandler("/api/blocks/spare",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qblock = getQueryParam(req, "block");
            auto qspare = getQueryParam(req, "spare");
            if (qblock.empty() || qspare.empty()) { cb(err(400, "?block and ?spare required")); return; }
            int64_t id = std::stoll(qblock);
            int64_t spare = std::stoll(qspare);
            if (!ft->setSpareBlock(id, spare)) { cb(err(404, "not found")); return; }
            cb(ok());
        }, {Patch});

    // ===================================================================
    // /api/objects — 对象级 API，数据存储到 block_path 文件系统文件
    // ===================================================================
    app().registerHandler("/api/objects",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {

            auto qpath = getQueryParam(req, "path");

            // POST: 创建对象
            if (req->getMethod() == Post) {
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                std::vector<uint8_t> data(req->bodyData(), req->bodyData() + req->bodyLength());
                auto result = ft->storeObject(qpath, data);
                if (!result) { cb(err(409, "exists or insert failed")); return; }
                auto r = HttpResponse::newHttpJsonResponse(fileToJson(*result));
                r->setStatusCode(k201Created); cb(r);
                return;
            }

            // GET: 获取对象数据
            if (req->getMethod() == Get) {
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                auto file = ft->getFile(qpath);
                if (!file) { cb(err(404, "not found")); return; }

                auto rangeHdr = req->getHeader("Range");
                if (!rangeHdr.empty()) {
                    uint64_t rs = 0, re = 0;
                    bool hasEnd = false;
                    if (!parseRange(rangeHdr, rs, re, hasEnd)) { cb(err(400, "invalid Range")); return; }
                    if (!hasEnd) re = file->size > 0 ? file->size - 1 : 0;
                    auto rr = ft->getObjectDataRange(qpath, rs, re);
                    if (!rr) { cb(err(500, "range read failed")); return; }
                    auto r = HttpResponse::newHttpResponse();
                    r->setStatusCode(k206PartialContent);
                    r->setContentTypeCode(CT_APPLICATION_OCTET_STREAM);
                    r->setBody(std::string(reinterpret_cast<const char*>(rr->data.data()), rr->data.size()));
                    std::ostringstream cr; cr << "bytes " << rr->rangeStart << "-" << rr->rangeEnd << "/" << rr->totalSize;
                    r->addHeader("Content-Range", cr.str());
                    r->addHeader("Accept-Ranges", "bytes");
                    cb(r); return;
                }

                auto data = ft->getObjectData(qpath);
                auto r = HttpResponse::newHttpResponse();
                r->setContentTypeCode(CT_APPLICATION_OCTET_STREAM);
                r->setBody(std::string(reinterpret_cast<const char*>(data.data()), data.size()));
                r->addHeader("Accept-Ranges", "bytes");
                cb(r); return;
            }

            // PATCH: 追加或指定偏移修改
            if (req->getMethod() == Patch) {
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                auto file = ft->getFile(qpath);
                if (!file) { cb(err(404, "not found")); return; }
                std::vector<uint8_t> data(req->bodyData(), req->bodyData() + req->bodyLength());
                auto offStr = getQueryParam(req, "offset");
                if (!offStr.empty()) {
                    uint64_t off = std::stoull(offStr);
                    auto result = ft->patchObjectData(qpath, off, data);
                    if (!result) { cb(err(500, "patch failed")); return; }
                    cb(HttpResponse::newHttpJsonResponse(fileToJson(*result))); return;
                }
                auto result = ft->appendObjectData(qpath, data);
                if (!result) { cb(err(500, "append failed")); return; }
                cb(HttpResponse::newHttpJsonResponse(fileToJson(*result))); return;
            }

            // PUT: 重命名
            if (req->getMethod() == Put) {
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                Json::Value body;
                Json::CharReaderBuilder b;
                std::string errs;
                std::istringstream s(std::string(req->bodyData(), req->bodyLength()));
                if (!Json::parseFromStream(b, s, &body, &errs)) { cb(err(400, "bad json")); return; }
                std::string newPath = body.get("new_path", "").asString();
                if (newPath.empty()) { cb(err(400, "new_path required")); return; }
                if (!ft->renameObject(qpath, newPath)) { cb(err(409, "rename failed")); return; }
                auto renamed = ft->getFile(newPath);
                if (!renamed) { cb(err(500, "internal error")); return; }
                cb(HttpResponse::newHttpJsonResponse(fileToJson(*renamed))); return;
            }

            // DELETE: 删除对象
            if (req->getMethod() == Delete) {
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                if (!ft->deleteObject(qpath)) { cb(err(404, "not found")); return; }
                cb(ok()); return;
            }

            cb(err(405, "method not allowed"));
        });

    LOG_INFO << "Server ready on " << listenAddr << ":" << listenPort;
    app().addListener(listenAddr, listenPort);
    app().run();
    return 0;
}
