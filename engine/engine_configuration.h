#pragma once

#ifndef ENGINE_PROJECT_NAME
#define ENGINE_PROJECT_NAME "kes"
#endif

#ifndef ENGINE_COMPANY_NAME
#define ENGINE_COMPANY_NAME "KES"
#endif

#ifndef ENGINE_CONTENT_DIRECTORY
#define ENGINE_CONTENT_DIRECTORY "content"
#endif

#ifndef ENGINE_PAK_ENCRYPTION_KEY_HEX
#define ENGINE_PAK_ENCRYPTION_KEY_HEX ""
#endif

#ifndef ENGINE_STARTUP_MAP
#define ENGINE_STARTUP_MAP "maps/startup.fmapc"
#endif

#ifndef ENGINE_ALLOW_DEBUG_MODE
#define ENGINE_ALLOW_DEBUG_MODE 1
#endif

#ifndef ENGINE_DEFAULT_PLAYER_CLASS_NAME
#define ENGINE_DEFAULT_PLAYER_CLASS_NAME "EnginePlayerEntity"
#endif

// Size of one map editor unit in engine units (meters). The default treats editor
// units: 1 unit = 1 inch, so a 72-unit-tall player is ~1.83 m.
#ifndef ENGINE_MAP_UNIT_SCALE
#define ENGINE_MAP_UNIT_SCALE 0.0254f
#endif
