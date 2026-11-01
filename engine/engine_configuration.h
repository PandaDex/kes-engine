#pragma once

// Engine Configuration defaults
#ifndef ENGINE_PROJECT_NAME
#define ENGINE_PROJECT_NAME "kes"
#endif

// Used for the per-user settings folder (SDL_GetPrefPath(company, project)).
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
#define ENGINE_STARTUP_MAP "maps/startup.map"
#endif

#ifndef ENGINE_ALLOW_DEBUG_MODE
#define ENGINE_ALLOW_DEBUG_MODE 1
#endif

#ifndef ENGINE_DEFAULT_PLAYER_CLASS_NAME
#define ENGINE_DEFAULT_PLAYER_CLASS_NAME "EnginePlayerEntity"
#endif
