#include "build_info.h"
#include "kes_build_info.h"

namespace
{
    struct ProjectInfo
    {
        std::string name = "kes";
        std::string company = "KES";
        std::string version = "1.0.0.0";
    };

    // Function-local so the generated project file can set it during static init.
    ProjectInfo& Project()
    {
        static ProjectInfo info;
        return info;
    }
}

void BuildInfoSetProject(const char* name, const char* company, const char* version)
{
    ProjectInfo& info = Project();
    if (name && name[0])
        info.name = name;
    if (company && company[0])
        info.company = company;
    if (version && version[0])
        info.version = version;
}

const char* BuildInfoName()
{
    return Project().name.c_str();
}

const char* BuildInfoCompany()
{
    return Project().company.c_str();
}

const char* BuildInfoVersion()
{
    return Project().version.c_str();
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
    return std::string(BuildInfoName()) + " " + BuildInfoVersion() + " (" KES_BUILD_COMMIT ", " KES_BUILD_CONFIG
        ", " KES_BUILD_PLATFORM ")";
}

std::string BuildInfoReleaseName()
{
    return std::string(BuildInfoName()) + "@" + BuildInfoVersion() + "+" KES_BUILD_COMMIT;
}
