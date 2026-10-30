#include "FormTagInterface.h"
#include "FileUtils.h"

#include "skse64/GameData.h"

#include <algorithm>

bool FormTagInterface::AddTag(TESForm* form, const char* tag)
{
    if (!form)
        return false;
    SimpleLocker locker(&m_tags.m_lock);
    auto& vec = m_tags.m_data[form->formID];
    SKEEFixedString t(tag);
    auto foundIt = std::find_if(vec.begin(), vec.end(), [&t](const SKEEFixedString& item) { return item == t; });
    if (foundIt != vec.end())
    {
        return false;
    }

    vec.push_back(tag);
    return true;
}

bool FormTagInterface::RemoveTag(TESForm* form, const char* tag)
{
    if (!form)
        return false;

    SimpleLocker locker(&m_tags.m_lock);
    auto it = m_tags.m_data.find(form->formID);
    if (it == m_tags.m_data.end())
    {
        return false;
    }

    auto foundIt = std::find(it->second.begin(), it->second.end(), tag);
    if (foundIt == it->second.end())
    {
        return false;
    }

    it->second.erase(foundIt);
    return true;
}

bool FormTagInterface::HasTags(TESForm* form)
{
    if (!form)
        return false;

    SimpleLocker locker(&m_tags.m_lock);
    auto it = m_tags.m_data.find(form->formID);
    if (it == m_tags.m_data.end())
    {
        return false;
    }

    return it->second.size() > 0;
}

bool FormTagInterface::HasTag(TESForm* form, const char* tag)
{
    if (!form)
        return false;

    SimpleLocker locker(&m_tags.m_lock);
    auto it = m_tags.m_data.find(form->formID);
    if (it == m_tags.m_data.end())
    {
        return false;
    }

    auto foundIt = std::find(it->second.begin(), it->second.end(), tag);
    if (foundIt == it->second.end())
    {
        return false;
    }

    return true;
}

void FormTagInterface::GetTags(TESForm* form, TagVisitor& visitor)
{
    if (!form)
        return;

    SimpleLocker locker(&m_tags.m_lock);
    auto it = m_tags.m_data.find(form->formID);
    if (it != m_tags.m_data.end())
    {
        for (auto& item : it->second)
        {
            visitor.Visit(item.c_str());
        }
    }
}

void FormTagInterface::GetForms(FormVisitor& visitor)
{
    SimpleLocker locker(&m_tags.m_lock);
    for (auto& item : m_tags.m_data)
    {
        visitor.Visit(LookupFormByID(item.first));
    }
}

bool FormTagInterface::AddPartTag(uint32_t partType, const char* tag, const char* label)
{
    SimpleLocker locker(&m_parts.m_lock);

    auto& vec = m_parts.m_data[partType];
    SKEEFixedString t(tag);
    auto foundIt = std::find_if(vec.begin(), vec.end(), [&t](const PartTag& item) { return item.name == t; });
    if (foundIt != vec.end())
    {
        foundIt->name = tag;
        foundIt->label = label;
        return true;
    }

    vec.push_back({tag, label});
    return true;
}

bool FormTagInterface::RemovePartTag(uint32_t partType, const char* tag)
{
    SimpleLocker locker(&m_parts.m_lock);
    auto it = m_parts.m_data.find(partType);
    if (it == m_parts.m_data.end())
    {
        return false;
    }

    SKEEFixedString t(tag);
    auto foundIt = std::find_if(it->second.begin(), it->second.end(), [&t](const PartTag& item) { return item.name == t; });
    if (foundIt == it->second.end())
    {
        return false;
    }

    it->second.erase(foundIt);
    return true;
}

bool FormTagInterface::HasPartTag(uint32_t partType, const char* tag)
{
    SimpleLocker locker(&m_parts.m_lock);

    auto it = m_parts.m_data.find(partType);
    if (it == m_parts.m_data.end())
    {
        return false;
    }

    SKEEFixedString t(tag);
    auto foundIt = std::find_if(it->second.begin(), it->second.end(), [&t](const PartTag& item) { return item.name == t; });
    if (foundIt == it->second.end())
    {
        return false;
    }

    return true;
}

bool FormTagInterface::HasPartTags(uint32_t partType)
{
    SimpleLocker locker(&m_parts.m_lock);
    auto it = m_parts.m_data.find(partType);
    if (it == m_parts.m_data.end())
    {
        return false;
    }

    return it->second.size() > 0;
}

void FormTagInterface::GetPartTags(uint32_t partType, PartTagVisitor& visitor)
{
    SimpleLocker locker(&m_parts.m_lock);
    auto it = m_parts.m_data.find(partType);
    if (it != m_parts.m_data.end())
    {
        for (auto& item : it->second)
        {
            visitor.Visit(item.name.c_str(), item.label.c_str());
        }
    }
}

void FormTagInterface::LoadMods()
{
    DataHandler* dataHandler = DataHandler::GetSingleton();
    if (dataHandler)
    {
        std::string fixedPath = "SKSE\\" + std::string("Plugins\\CharGen\\Tags\\");

        ForEachMod([&](ModInfo* modInfo)
        {
            std::string templatesPath = fixedPath + std::string(modInfo->name) + "\\tags.json";
            LoadTagMod(templatesPath);
        });
        ForEachMod([&](ModInfo* modInfo)
        {
            std::string templatesPath = fixedPath + std::string(modInfo->name) + "\\parts.json";
            LoadPartTagMod(templatesPath);
        });
    }
}

#include <json/json.h>

bool FormTagInterface::LoadTagMod(const std::string& path)
{
    bool loadError = false;
    BSResourceNiBinaryStream file(path.c_str());
    if (!file.IsValid()) {
        loadError = true;
        return loadError;
    }

    std::string in;
    BSFileUtil::ReadAll(&file, in);

    Json::Features features;
    features.all();

    Json::Value root;
    Json::Reader reader(features);

    bool parseSuccess = reader.parse(in, root);
    if (!parseSuccess) {
        _ERROR("%s: Error occured parsing json for %s.", __FUNCTION__, path.c_str());
        loadError = true;
        return loadError;
    }

    try
    {
        auto AddTagFromForm = [this](const std::string& identifier, const char* tag) {
            TESForm* form = GetFormFromIdentifier(identifier);
            AddTag(form, tag);
        };

        auto members = root.getMemberNames();
        for (auto& item : members)
        {
            if (root[item].isArray())
            {
                for (auto& child : root[item])
                {
                    AddTagFromForm(item, child.asString().c_str());
                }
            }
            else if (root[item].isString())
            {
                AddTagFromForm(item, root[item].asString().c_str());
            }
        }
    }
    catch (const std::exception& err)
    {
        _ERROR("%s Failed to load json: %s\n%s", path.c_str(), err.what());
        loadError = true;
    }
    
    return loadError;
}


bool FormTagInterface::LoadPartTagMod(const std::string& path)
{
    bool loadError = false;
    BSResourceNiBinaryStream file(path.c_str());
    if (!file.IsValid()) {
        loadError = true;
        return loadError;
    }

    std::string in;
    BSFileUtil::ReadAll(&file, in);

    Json::Features features;
    features.all();

    Json::Value root;
    Json::Reader reader(features);

    bool parseSuccess = reader.parse(in, root);
    if (!parseSuccess) {
        _ERROR("%s: Error occured parsing json for %s.", __FUNCTION__, path.c_str());
        loadError = true;
        return loadError;
    }

    try
    {
        for (auto& parts : root["parts"])
        {
            if (parts.isObject())
            {
                uint32_t partType = parts["type"].asUInt();
                for (auto& tags : parts["tags"])
                {
                    if (tags.isObject())
                    {
                        AddPartTag(partType, tags["name"].asString().c_str(), tags["label"].asString().c_str());
                    }
                    else if (tags.isString())
                    {
                        auto tag = tags.asString();
                        AddPartTag(partType, tag.c_str(), tag.c_str());
                    }
                }
            }
        }
    }
    catch (const std::exception& err)
    {
        _ERROR("%s Failed to load json: %s\n%s", path.c_str(), err.what());
        loadError = true;
    }

    return loadError;
}