#include "test_framework.h"
#include <sstream>
#include "logger.h"

TEST_CASE("logger: the log file can be read while it is open")
{
    kestest::TempDir dir;
    REQUIRE(LoggerInitialize(dir.Root().string()));
    EngineLog("readable while running");

    const std::string path = GetCurrentLogFilePath();
    REQUIRE(!path.empty());

    std::ifstream file(path);
    CHECK(file.is_open());
    std::stringstream contents;
    contents << file.rdbuf();
    CHECK(contents.str().find("readable while running") != std::string::npos);
    file.close();

    LoggerShutdown();
}
