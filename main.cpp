#include "FileTable.h"
#include <drogon/drogon.h>
#include <json/value.h>
#include <json/writer.h>
#include <sstream>
#include <iostream>

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

// Helper: get "path" from query string
std::string getQueryParam(const HttpRequestPtr& req, const std::string& key) {
    auto& params = req->getParameters();
    auto it = params.find(key);
    return it != params.end() ? it->second : "";
}

} // anonymous namespace

// ===================================================================
// Main
// ===================================================================

int main()
{
    auto ft = std::make_shared<FileTable>("storage.db");

    // Web UI — static HTML with inline JS (split to avoid MSVC line-length limits)
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

    // GET/POST /api/files
    app().registerHandler("/api/files",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {

            if (req->getMethod() == Get) {
                // GET /api/files?path=...  or  GET /api/files (list all)
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
                // POST /api/files  — create
                Json::Value body;
                Json::CharReaderBuilder b;
                std::string errs;
                std::istringstream s(std::string(req->bodyData(), req->bodyLength()));
                if (!Json::parseFromStream(b, s, &body, &errs))
                    { cb(err(400, "bad json")); return; }
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
                r->setStatusCode(k201Created);
                cb(r);
                return;
            }

            if (req->getMethod() == Put) {
                // PUT /api/files?path=...  — update
                auto qpath = getQueryParam(req, "path");
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                auto f = ft->getFile(qpath);
                if (!f) { cb(err(404, "not found")); return; }
                Json::Value body;
                Json::CharReaderBuilder b;
                std::string errs;
                std::istringstream s(std::string(req->bodyData(), req->bodyLength()));
                if (!Json::parseFromStream(b, s, &body, &errs))
                    { cb(err(400, "bad json")); return; }
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
                // DELETE /api/files?path=...
                auto qpath = getQueryParam(req, "path");
                if (qpath.empty()) { cb(err(400, "?path required")); return; }
                if (!ft->deleteFile(qpath)) { cb(err(404, "not found")); return; }
                cb(ok());
                return;
            }

            cb(err(405, "method not allowed"));
        });

    // GET /api/files/blocks?path=...  — get blocks of a file
    app().registerHandler("/api/files/blocks",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qpath = getQueryParam(req, "path");
            if (qpath.empty()) { cb(err(400, "?path required")); return; }
            Json::Value arr(Json::arrayValue);
            for (auto& b : ft->getFileBlocks(qpath))
                arr.append(blockToJson(b));
            cb(HttpResponse::newHttpJsonResponse(arr));
        },
        {Get});

    // POST /api/files/blocks?path=...&block=N  — append block to file
    app().registerHandler("/api/files/blocks",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qpath  = getQueryParam(req, "path");
            auto qblock = getQueryParam(req, "block");
            if (qpath.empty() || qblock.empty())
                { cb(err(400, "?path and ?block required")); return; }
            int64_t bid = std::stoll(qblock);
            if (!ft->appendBlockToFile(qpath, bid))
                { cb(err(404, "file or block not found")); return; }
            cb(ok());
        },
        {Post});

    // POST /api/blocks  — create
    app().registerHandler("/api/blocks",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            Json::Value body;
            Json::CharReaderBuilder b;
            std::string errs;
            std::istringstream s(std::string(req->bodyData(), req->bodyLength()));
            if (!Json::parseFromStream(b, s, &body, &errs))
                { cb(err(400, "bad json")); return; }
            BlockEntry bk;
            bk.blockPath   = body.get("block_path","").asString();
            bk.sha256      = body.get("sha256","").asString();
            bk.blockSize   = body.get("block_size", Json::Value(0)).asInt64();
            bk.isBadBlock  = body.get("is_bad", false).asBool();
            bk.nextBlockId  = -1;
            bk.spareBlockId = -1;
            if (bk.blockPath.empty() || bk.sha256.empty())
                { cb(err(400, "block_path, sha256, block_size required")); return; }
            int64_t id = ft->insertBlock(bk);
            if (id < 0) { cb(err(500, "insert failed")); return; }
            bk.id = id;
            auto r = HttpResponse::newHttpJsonResponse(blockToJson(bk));
            r->setStatusCode(k201Created);
            cb(r);
        },
        {Post});

    // PUT /api/blocks?block=N  — update
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
            if (!Json::parseFromStream(b, s, &body, &errs))
                { cb(err(400, "bad json")); return; }
            if (body.isMember("block_path"))  bk->blockPath  = body["block_path"].asString();
            if (body.isMember("sha256"))      bk->sha256     = body["sha256"].asString();
            if (body.isMember("block_size"))  bk->blockSize  = body["block_size"].asInt64();
            if (body.isMember("is_bad"))      bk->isBadBlock = body["is_bad"].asBool();
            if (body.isMember("next_block"))  bk->nextBlockId = body["next_block"].asInt64();
            if (body.isMember("spare_block")) bk->spareBlockId = body["spare_block"].asInt64();
            ft->updateBlock(*bk);
            cb(HttpResponse::newHttpJsonResponse(blockToJson(*bk)));
        },
        {Put});

    // DELETE /api/blocks/delete?block=N
    app().registerHandler("/api/blocks/delete",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qblock = getQueryParam(req, "block");
            if (qblock.empty()) { cb(err(400, "?block required")); return; }
            int64_t id = std::stoll(qblock);
            if (!ft->deleteBlock(id)) { cb(err(404, "not found")); return; }
            cb(ok());
        },
        {Delete});

    // PATCH /api/blocks/bad?block=N  — mark bad
    app().registerHandler("/api/blocks/bad",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qblock = getQueryParam(req, "block");
            if (qblock.empty()) { cb(err(400, "?block required")); return; }
            int64_t id = std::stoll(qblock);
            if (!ft->markBadBlock(id)) { cb(err(404, "not found")); return; }
            cb(ok());
        },
        {Patch});

    // PATCH /api/blocks/spare?block=N&spare=M  — set spare
    app().registerHandler("/api/blocks/spare",
        [ft](const HttpRequestPtr& req,
             std::function<void(const HttpResponsePtr&)>&& cb) {
            auto qblock = getQueryParam(req, "block");
            auto qspare = getQueryParam(req, "spare");
            if (qblock.empty() || qspare.empty())
                { cb(err(400, "?block and ?spare required")); return; }
            int64_t id    = std::stoll(qblock);
            int64_t spare = std::stoll(qspare);
            if (!ft->setSpareBlock(id, spare)) { cb(err(404, "not found")); return; }
            cb(ok());
        },
        {Patch});

    std::cout << "Server ready on port 8080" << std::endl;
    app().addListener("0.0.0.0", 8080);
    app().run();
    return 0;
}
