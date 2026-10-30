#pragma once

#include "IPluginInterface.h"
#include "StringTable.h"

#include <mutex>
#include <vector>

class FormTagMap : public SafeDataHolder<std::unordered_map<uint32_t, std::vector<SKEEFixedString>>>
{
public:
	friend class FormTagInterface;
};

struct PartTag
{
	SKEEFixedString name;
	SKEEFixedString label;
};

class PartTagMap : public SafeDataHolder<std::unordered_map<uint32_t, std::vector<PartTag>>>
{
public:
	friend class FormTagInterface;
};

class FormTagInterface : public IFormTagInterface
{
public:
    virtual skee_u32 GetVersion() override { return kCurrentPluginVersion; }
    virtual void Revert() { };

	virtual bool AddTag(TESForm* form, const char* tag) override;
	virtual bool RemoveTag(TESForm* form, const char* tag) override;
	virtual bool HasTags(TESForm* form) override;
	virtual bool HasTag(TESForm* form, const char* tag) override;

	// Visits all Tags on a given EditorID
	virtual void GetTags(TESForm* form, TagVisitor& visitor) override;

	// Visits all EditorIDs which have tags
	virtual void GetForms(FormVisitor& visitor) override;

	virtual bool AddPartTag(uint32_t partType, const char* tag, const char* label) override;
	virtual bool RemovePartTag(uint32_t partType, const char* tag) override;
	virtual bool HasPartTags(uint32_t partType) override;
	virtual bool HasPartTag(uint32_t partType, const char* tag) override;

	virtual void GetPartTags(uint32_t partType, PartTagVisitor& visitor) override;

	void LoadMods();
	bool LoadTagMod(const std::string& path);
	bool LoadPartTagMod(const std::string& path);

private:
	FormTagMap m_tags;
	PartTagMap m_parts;
};