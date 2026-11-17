// QOA codec implementation (external/qoa), compiled as C: the header relies on
// implicit void* conversions. Warnings are silenced since it is third-party.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#pragma warning(disable: 4701)
#endif
#define QOA_IMPLEMENTATION
#define QOA_NO_STDIO
#include <qoa.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
