#include "entity_factory_registry.h"
#include "entity_manager.h"

void EntityFactoryRegistry::Clear()
{
    factories.clear();
}

void EntityFactoryRegistry::Register(const std::string& className, EntityFactoryFunction factory)
{
    if (className.empty() || !factory)
        return;

    factories[className] = std::move(factory);
}

Entity* EntityFactoryRegistry::Create(const std::string& className, EntityManager& manager, GameContext& context) const
{
    (void)context;

    const auto it = factories.find(className);
    if (it == factories.end())
        return nullptr;

    return it->second(manager, context);
}

bool EntityFactoryRegistry::Has(const std::string& className) const
{
    return factories.find(className) != factories.end();
}
