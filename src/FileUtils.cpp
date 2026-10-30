#include "FileUtils.h"

#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <Shlwapi.h>

#include "skse64/GameStreams.h"
#include "skse64/GameData.h"
#include "skse64/GameForms.h"
#include "skse64/GameRTTI.h"

// trim from start
namespace std
{
	std::string &ltrim(std::string &s) {
		s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int s) { return !std::isspace(s); }));
		return s;
	}

	// trim from end
	std::string &rtrim(std::string &s) {
		s.erase(std::find_if(s.rbegin(), s.rend(), [](int s) { return !std::isspace(s); }).base(), s.end());
		return s;
	}

	// trim from both ends
	std::string &trim(std::string &s) {
		return ltrim(rtrim(s));
	}

	// return vector of delimited string
	std::vector<std::string> explode(const std::string& str, const char& ch) {
		std::string next;
		std::vector<std::string> result;

		for (std::string::const_iterator it = str.begin(); it != str.end(); it++) {
			if (*it == ch) {
				if (!next.empty()) {
					result.push_back(next);
					next.clear();
				}
			}
			else {
				next += *it;
			}
		}
		if (!next.empty())
			result.push_back(next);
		return result;
	}
}

namespace BSFileUtil
{
bool ReadLine(BSResourceNiBinaryStream* fin, std::string* str)
{
	char buf[1024];
	UInt32 ret = 0;

	ret = fin->ReadLine((char*)buf, 1024, '\n');
	if (ret > 0) {
		buf[ret - 1] = '\0';
		*str = buf;
		return true;
	}
	return false;
}
}

namespace FileUtils
{
void GetAllFiles(LPCTSTR lpFolder, LPCTSTR lpFilePattern, std::vector<SKEEFixedString> & filePaths)
{
	TCHAR szFullPattern[MAX_PATH];
	WIN32_FIND_DATA FindFileData;
	HANDLE hFindFile;
	// first we are going to process any subdirectories
	PathCombine(szFullPattern, lpFolder, "*");
	hFindFile = FindFirstFile(szFullPattern, &FindFileData);
	if (hFindFile != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				// found a subdirectory; recurse into it
				PathCombine(szFullPattern, lpFolder, FindFileData.cFileName);
				if (FindFileData.cFileName[0] == '.')
					continue;
				GetAllFiles(szFullPattern, lpFilePattern, filePaths);
			}
		} while (FindNextFile(hFindFile, &FindFileData));
		FindClose(hFindFile);
	}
	// now we are going to look for the matching files
	PathCombine(szFullPattern, lpFolder, lpFilePattern);
	hFindFile = FindFirstFile(szFullPattern, &FindFileData);
	if (hFindFile != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				// found a file; do something with it
				PathCombine(szFullPattern, lpFolder, FindFileData.cFileName);
				filePaths.push_back(szFullPattern);
			}
		} while (FindNextFile(hFindFile, &FindFileData));
		FindClose(hFindFile);
	}
}
}

TESRace * GetRaceByName(std::string & raceName)
{
	DataHandler * dataHandler = DataHandler::GetSingleton();
	if (dataHandler)
	{
		for (UInt32 i = 0; i < dataHandler->races.count; i++)
		{
			TESRace * race = NULL;
			if (dataHandler->races.GetNthItem(i, race)) {
				BSFixedString raceStrName(raceName.c_str());
				if (race->editorId == raceStrName) {
					raceStrName.Release();
					return race;
				}
				raceStrName.Release();
			}
		}
	}

	return NULL;
}

// Keyed by the string pool pointer, which is what BSFixedString compares. Read from job threads, only built at data load.
static absl::flat_hash_map<const char *, BGSHeadPart *> s_headPartsByName;
static UInt32 s_indexedHeadParts = 0;

void BuildHeadPartIndex()
{
	DataHandler * dataHandler = DataHandler::GetSingleton();
	if (!dataHandler)
		return;

	s_headPartsByName.reserve(dataHandler->headParts.count);
	for (UInt32 i = 0; i < dataHandler->headParts.count; i++)
	{
		BGSHeadPart * headPart = NULL;
		if (dataHandler->headParts.GetNthItem(i, headPart) && headPart)
			s_headPartsByName.emplace(headPart->partName.data, headPart); // Keeps the first match in load order
	}
	s_indexedHeadParts = dataHandler->headParts.count;
}

BGSHeadPart * GetHeadPartByName(std::string & headPartName)
{
	DataHandler * dataHandler = DataHandler::GetSingleton();
	if (!dataHandler)
		return NULL;

	BGSHeadPart * result = NULL;
	BSFixedString partName(headPartName.c_str());
	if (s_indexedHeadParts > 0 && s_indexedHeadParts == dataHandler->headParts.count)
	{
		auto it = s_headPartsByName.find(partName.data);
		if (it != s_headPartsByName.end())
			result = it->second;
	}
	else
	{
		for (UInt32 i = 0; i < dataHandler->headParts.count; i++)
		{
			BGSHeadPart * headPart = NULL;
			if (dataHandler->headParts.GetNthItem(i, headPart) && headPart && headPart->partName == partName) {
				result = headPart;
				break;
			}
		}
	}

	partName.Release();
	return result;
}

ModInfo* GetModInfoByFormID(UInt32 formId, bool allowLight)
{
	DataHandler * dataHandler = DataHandler::GetSingleton();

	UInt8 modIndex = formId >> 24;
	UInt16 lightIndex = ((formId >> 12) & 0xFFF);

	ModInfo* modInfo = nullptr;
	if (modIndex == 0xFE && allowLight) {
		if (lightIndex < dataHandler->modList.loadedCCMods.count)
			dataHandler->modList.loadedCCMods.GetNthItem(lightIndex, modInfo);
	} else {
		if(modIndex < 0xFE)
			dataHandler->modList.loadedMods.GetNthItem(modIndex, modInfo);
	}

	return modInfo;
}

std::string GetFormIdentifier(TESForm * form)
{
	char formName[MAX_PATH];
	UInt8 modIndex = form->formID >> 24;
	UInt32 modForm = form->formID & 0xFFFFFF;

	ModInfo* modInfo = nullptr;
	if (modIndex == 0xFE)
	{
		UInt16 lightIndex = (form->formID >> 12) & 0xFFF;
		if (lightIndex < (*g_dataHandler)->modList.loadedCCMods.count)
			modInfo = (*g_dataHandler)->modList.loadedCCMods[lightIndex];
	}
	else
	{
		modInfo = (*g_dataHandler)->modList.loadedMods[modIndex];
	}

	if (modInfo) {
		sprintf_s(formName, "%s|%06X", modInfo->name, modForm);
	}

	return formName;
}

// SKSE 2.0.20 doesn't declare this. Unlike later versions it takes the name by reference.
typedef TESForm * (*_LookupFormByEditorID)(const char * const & editorId);
static RelocAddr<_LookupFormByEditorID> LookupFormByEditorID(0x001942E0);

TESForm * GetFormFromIdentifier(const std::string & formIdentifier)
{
	std::size_t pos = formIdentifier.find_first_of('|');
	if (pos == std::string::npos) {
		return LookupFormByEditorID(formIdentifier.c_str());
	}
	std::string modName = formIdentifier.substr(0, pos);
	std::string modForm = formIdentifier.substr(pos + 1);

	UInt32 formId = 0;
	sscanf_s(modForm.c_str(), "%X", &formId);

	const ModInfo * modInfo = (*g_dataHandler)->LookupModByName(modName.c_str());
	if (!modInfo || !modInfo->IsActive()) {
		return nullptr;
	}

	return LookupFormByID(modInfo->GetFormID(formId));
}

void VisitLeveledCharacter(TESLevCharacter * character, std::function<void(TESNPC*)> functor)
{
	absl::flat_hash_set<TESLevCharacter*> visited;
	std::queue<TESLevCharacter*> visit;

	visit.push(character);

	while (!visit.empty())
	{
		character = visit.front();
		visit.pop();

		if (character)
		{
			for (UInt32 i = 0; i < character->leveledList.length; i++)
			{
				TESForm * form = character->leveledList.entries[i].form;
				if (form) {
					TESLevCharacter * levCharacter = DYNAMIC_CAST(form, TESForm, TESLevCharacter);
					if (levCharacter && visited.find(levCharacter) == visited.end())
						visit.push(levCharacter);

					TESNPC * npc = DYNAMIC_CAST(form, TESForm, TESNPC);
					if (npc)
						functor(npc);
				}
			}

			visited.insert(character);
		}
	}
}

void ForEachMod(std::function<void(ModInfo *)> functor)
{
	class ActiveModVisitor
	{
	public:
		ActiveModVisitor(std::function<void(ModInfo *)> fn) : m_function(fn) { }

		bool Accept(ModInfo* modInfo)
		{
			if (modInfo->IsActive())
			{
				m_function(modInfo);
			}
			return true;
		}

		std::function<void(ModInfo *)> m_function;
	};

	ActiveModVisitor visitor(functor);
	(*g_dataHandler)->modList.modInfoList.Visit(visitor);
}