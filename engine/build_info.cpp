#include "build_info.h"
#include "kes_build_info.h"

const char* BuildInfoVersion()
{
    return KES_BUILD_VERSION;
}

const char* BuildInfoCommit()
{
    return KES_BUILD_COMMIT;
}

const char* BuildInfoConfig()
{
    return KES_BUILD_CONFIG;
}

std::string BuildInfoDescription()
{
    return KES_BUILD_NAME " " KES_BUILD_VERSION " (" KES_BUILD_COMMIT ", " KES_BUILD_CONFIG ", " KES_BUILD_PLATFORM ")";
}

std::string BuildInfoReleaseName()
{
    return KES_BUILD_NAME "@" KES_BUILD_VERSION "+" KES_BUILD_COMMIT;
}
