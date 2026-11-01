#pragma once
#include <functional>
#include <string>
#include <unordered_map>

class Entity;
class EntityManager;
struct GameContext;

using EntityFactoryFunction = std::function<Entity*(EntityManager&, GameContext&)>;

class EntityFactoryRegistry
{
public:
    void Clear();
    void Register(const std::string& className, EntityFactoryFunction factory);
    Entity* Create(const std::string& className, EntityManager& manager, GameContext& context) const;
    bool Has(const std::string& className) const;

private:
    std::unordered_map<std::string, EntityFactoryFunction> factories;
};
