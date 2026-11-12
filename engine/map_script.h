#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "guid.h"

class Entity;
struct GameContext;

// MapScript: the editor's output -> input links (Outputs tab in Forge).
//
// An entity takes part by implementing IMapScriptable next to Entity:
//
//   class DoorEntity : public Entity, public IMapScriptable
//   {
//       const MapScriptPorts& GetMapScriptPorts() const override
//       {
//           static const MapScriptPorts ports = { { { "Open" }, { "Close" } }, { { "OnOpened" } } };
//           return ports;
//       }
//       bool AcceptInput(const std::string& input, const MapScriptCall& call) override
//       {
//           if (input == "Open") { Open(); return true; }
//           ...
//           return false;
//       }
//   };
//
// and fires its outputs with Entity::FireOutput("OnOpened", activator). Links target entities
// by GUID (Entity::GetGuid), so they survive renames; the ports declared here should match the
// class's input/output lines in the editor's .dec file. Every scriptable entity also takes the
// built-in "Kill" input.

struct MapScriptPort
{
    std::string name;
    std::string type = "void";   // parameter type, as in the .dec: void, string, float, integer, bool
};

struct MapScriptPorts
{
    std::vector<MapScriptPort> inputs;
    std::vector<MapScriptPort> outputs;

    bool HasInput(std::string_view name) const;
    bool HasOutput(std::string_view name) const;
};

// What an input receives. Entities may be null: the caller or activator can be gone by the
// time a delayed link arrives.
struct MapScriptCall
{
    Entity* caller = nullptr;      // entity whose output fired
    Entity* activator = nullptr;   // entity that started the chain (e.g. the player in a trigger)
    std::string value;             // link parameter, or the value the output fired with
};

class IMapScriptable
{
public:
    virtual ~IMapScriptable() = default;
    virtual const MapScriptPorts& GetMapScriptPorts() const = 0;
    // Return false for inputs the entity doesn't handle (logged as a warning).
    virtual bool AcceptInput(const std::string& input, const MapScriptCall& call) = 0;
};

struct MapScriptLink
{
    std::string output;
    Guid target;
    std::string input;
    std::string parameter;   // "" = pass the output's value through
    float delay = 0.0f;      // seconds
    bool once = false;       // removed after it fires
};

// Owns the links of the current map and delivers inputs. Outputs are queued and delivered in
// Update() (after entity updates), never inside the code that fired them; an input fired while
// delivering runs on the next Update, so link loops can't hang a frame.
class MapScriptSystem
{
public:
    void SetGameContext(GameContext* ctx) { game = ctx; }
    void Clear();

    void AddLink(const Guid& source, MapScriptLink link);
    size_t GetLinkCount() const;
    const std::vector<MapScriptLink>* GetLinks(const Guid& source) const;

    void FireOutput(Entity& source, std::string_view output, Entity* activator = nullptr, std::string_view value = {});
    // Sends an input now, bypassing links (game code, console). False when the target doesn't
    // take MapScript inputs or rejects this one.
    bool SendInput(Entity& target, const std::string& input, Entity* caller = nullptr,
                   Entity* activator = nullptr, const std::string& value = {});

    void Update(float dt);
    size_t GetPendingEventCount() const { return pending.size(); }
    // True while Update delivers inputs (World::LoadMap queues the load meanwhile).
    bool IsDispatching() const { return dispatching; }
    double GetTime() const { return time; }

private:
    struct Event
    {
        double dueTime = 0.0;
        uint64_t order = 0;   // keeps events due at the same time in firing order
        Guid target;
        Guid caller;
        Guid activator;
        std::string input;
        std::string value;
    };

    Entity* Find(const Guid& guid) const;

    GameContext* game = nullptr;
    std::unordered_map<Guid, std::vector<MapScriptLink>, GuidHash> links;
    std::vector<Event> pending;
    double time = 0.0;
    uint64_t nextOrder = 0;
    uint64_t generation = 0;   // bumped by Clear(), ends a delivery the level was cleared in
    bool dispatching = false;
};
