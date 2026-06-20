#include "FileTable.h"
#include <iostream>
#include <drogon/drogon.h>

int main()
{
    using namespace enostorg;

    FileTable ft("test.db");

    BlockEntry b1("/blocks/001.bin", -1, -1, false, 4096,
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    int64_t b1id = ft.insertBlock(b1);

    BlockEntry b2("/blocks/002.bin", -1, -1, false, 2048,
                  "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899");
    int64_t b2id = ft.insertBlock(b2);

    FileEntry f("/docs/doc.txt", std::time(nullptr), std::time(nullptr),
                6144, "document", b1id);
    ft.insertFile(f);
    ft.appendBlockToFile(f.filePath, b2id);

    auto blocks = ft.getFileBlocks(f.filePath);
    std::cout << "file " << f.filePath << " has "
              << blocks.size() << " blocks" << std::endl;

    drogon::app().addListener("0.0.0.0", 8080);
    drogon::app().run();
    return 0;
}
