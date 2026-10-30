#pragma once

#include "IPluginInterface.h"
#include "IHashType.h"

#include "skse64/GameTypes.h"
#include "skse64/GameThreads.h"
#include "skse64/NiTypes.h"
#include "skse64/NiGeometry.h"

#include "StringTable.h"
#include "MorphKernel.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <half.hpp>

#include <string>
#include <set>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <chrono>
#include <ctime>
#include <mutex>

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

class TESObjectREFR;
struct SKSESerializationInterface;
class BSResourceNiBinaryStream;
class NiAVObject;
class TESObjectARMO;
class TESObjectARMA;
class NiExtraData;
class TESNPC;
class TESRace;
class NiSkinPartition;
struct ModInfo;
class TESFaction;

#define MORPH_MOD_DIRECTORY "actors\\character\\BodyGenData\\"

class BodyMorph
{
public:
	bool operator<(const BodyMorph & rhs) const { return *m_name < *rhs.m_name; }
	bool operator==(const BodyMorph & rhs) const	{ return *m_name == *rhs.m_name; }

	StringTableItem	m_name;
	float			m_value;

	void Save(SKSESerializationInterface * intfc, UInt32 kVersion);
	bool Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable);
};

class BodyMorphSet : public absl::btree_set<BodyMorph>
{
public:
	// Serialization
	void Save(SKSESerializationInterface * intfc, UInt32 kVersion);
	bool Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable);
};

class BodyMorphData : public absl::node_hash_map<StringTableItem, absl::flat_hash_map<StringTableItem, float>>
{
public:
	void Save(SKSESerializationInterface * intfc, UInt32 kVersion);
	bool Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable);
};

typedef UInt32 MorphKey;
class ActorMorphs : public SafeDataHolder<absl::node_hash_map<MorphKey, BodyMorphData>>
{
	friend class BodyMorphInterface;
public:
	typedef absl::node_hash_map<MorphKey, BodyMorphData>	MorphMap;

	// Serialization
	void Save(SKSESerializationInterface * intfc, UInt32 kVersion);
	bool Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable);
};

class TriShapeVertexDelta
{
public:
	UInt16		index;
	DirectX::XMVECTOR delta;
};

// Same layout as the TRI file, a body has millions of these
class TriShapePackedVertexDelta
{
public:
	UInt16	index;
	SInt16	x;
	SInt16	y;
	SInt16	z;
};
static_assert(sizeof(TriShapePackedVertexDelta) == 8);

class TriShapePackedUVDelta
{
public:
	UInt16	index;
	SInt16	u;
	SInt16	v;
};

class TriShapeVertexData
{
public:
	virtual void ApplyMorphRaw(UInt16 vertCount, void * vertices, float factor) = 0;

	struct Layout
	{
		UInt64	vertexDesc;
		UInt8	* vertexData;
	};

	virtual void ApplyMorph(UInt16 vertexCount, Layout * vertexData, float factor) = 0;
	virtual size_t GetSize() const = 0;
};
typedef std::shared_ptr<TriShapeVertexData> TriShapeVertexDataPtr;

class TriShapeFullVertexData : public TriShapeVertexData
{
public:
	TriShapeFullVertexData() : m_maxIndex(0) { }

	virtual void ApplyMorphRaw(UInt16 vertCount, void * vertices, float factor);
	virtual void ApplyMorph(UInt16 vertexCount, Layout * vertexData, float factor);
	virtual size_t GetSize() const { return m_vertexDeltas.size(); }

	UInt32								m_maxIndex;
	std::vector<TriShapeVertexDelta>	m_vertexDeltas;
};
typedef std::shared_ptr<TriShapeFullVertexData> TriShapeFullVertexDataPtr;

class TriShapePackedVertexData : public TriShapeVertexData
{
public:
	TriShapePackedVertexData() : m_maxIndex(0) { }

	virtual void ApplyMorphRaw(UInt16 vertCount, void * vertices, float factor);
	virtual void ApplyMorph(UInt16 vertexCount, Layout* vertexData, float factor);
	virtual size_t GetSize() const { return m_vertexDeltas.size(); }

	float									m_multiplier;
	UInt32									m_maxIndex;
	std::vector<TriShapePackedVertexDelta>	m_vertexDeltas;
};
typedef std::shared_ptr<TriShapePackedVertexData> TriShapePackedVertexDataPtr;

class TriShapePackedUVData : public TriShapeVertexData
{
public:
	TriShapePackedUVData() : m_maxIndex(0) { }

	struct UVCoord
	{
		half_float::half u;
		half_float::half v;
	};

	virtual void ApplyMorphRaw(UInt16 vertCount, void * vertices, float factor);
	virtual void ApplyMorph(UInt16 vertexCount, Layout* vertexData, float factor);
	virtual size_t GetSize() const { return m_uvDeltas.size(); }

	float								m_multiplier;
	UInt32								m_maxIndex;
	std::vector<TriShapePackedUVDelta>	m_uvDeltas;
};
typedef std::shared_ptr<TriShapePackedUVData> TriShapePackedUVDataPtr;


class BodyMorphMap : public absl::flat_hash_map<SKEEFixedString, std::pair<TriShapeVertexDataPtr, TriShapeVertexDataPtr>>
{
	friend class MorphCache;
public:
	BodyMorphMap() : m_hasUV(false), m_topologies(std::make_shared<Topologies>()) { }

	void ForEachMorph(std::function<void(const SKEEFixedString&, const std::pair<TriShapeVertexDataPtr, TriShapeVertexDataPtr>&)> functor) const;

	// Finds or builds the topology of the mesh these morphs are being applied to, from its unmorphed vertex block
	const MorphKernel::Topology * GetTopology(const UInt8 * vertices, UInt32 vertexCount, const MorphKernel::VertexLayout & layout, const std::vector<MorphKernel::TriangleRun> & triangles, const std::vector<UInt16> & lockedVertices) const;

	bool HasUV() const { return m_hasUV; }

private:
	// Usually a single mesh, but nothing stops two NIFs from sharing a TRI
	struct Topologies
	{
		std::mutex lock;
		std::vector<std::unique_ptr<MorphKernel::Topology>> meshes;
	};

	bool m_hasUV;
	std::shared_ptr<Topologies> m_topologies;
};

class TriShapeMap : public absl::node_hash_map<SKEEFixedString, BodyMorphMap>
{
public:
	TriShapeMap()
	{
		memoryUsage = sizeof(TriShapeMap);
	}

	size_t memoryUsage;
};

struct MorphJob;
typedef std::vector<std::pair<StringTableItem, float>> MorphFactors;

class MorphFileCache
{
	friend class MorphCache;
	friend class BodyMorphInterface;
public:
	void CollectJobs(NiAVObject * rootNode, bool isAttaching, std::vector<MorphJob> & jobs);
	void ForEachShape(std::function<void(const SKEEFixedString&, const BodyMorphMap&)> functor) const;

	size_t GetByteSize() const { return vertexMap.memoryUsage; }

private:
#ifdef _DEBUG
	SKEEFixedString path;
#endif
	TriShapeMap vertexMap;
	std::time_t accessed;
};

class MorphCache : public SafeDataHolder<absl::node_hash_map<SKEEFixedString, MorphFileCache>>
{
	friend class BodyMorphInterface;

public:
	MorphCache()
	{
		totalMemory = sizeof(MorphCache);
		memoryLimit = totalMemory;
	}

	typedef absl::node_hash_map<SKEEFixedString, TriShapeMap>	FileMap;

	SKEEFixedString CreateTRIPath(const char * relativePath);
	bool CacheFile(const char * modelPath);

	void ApplyMorphs(TESObjectREFR * refr, NiAVObject * rootNode, bool attaching = false, bool deferUpdate = false);
	void UpdateMorphs(TESObjectREFR * refr, bool deferUpdate = false);
	void ForEachMorphFile(std::function<void(const SKEEFixedString&, const MorphFileCache&)> functor) const;

	void Shrink();
	size_t Clear();

private:
	void ApplyMorphsNow(TESObjectREFR * refr, NiAVObject * rootNode, bool attaching);
	void UpdateMorphsNow(TESObjectREFR * refr);
	void CollectJobs(NiAVObject * rootNode, bool attaching, std::vector<MorphJob> & jobs);
	void RunJobs(TESObjectREFR * refr, std::vector<MorphJob> & jobs, std::chrono::steady_clock::time_point start);

	size_t memoryLimit;
	size_t totalMemory;
	std::vector<UInt8> m_vertexBlocks;
};

class NIOVTaskUpdateModelWeight : public TaskDelegate
{
public:
	virtual void Run();
	virtual void Dispose();

	NIOVTaskUpdateModelWeight(Actor * actor);
	
private:
	UInt32	m_formId;
};

class NIOVTaskUpdateSkinPartition : public TaskDelegate
{
public:
	virtual void Run();
	virtual void Dispose();

	NIOVTaskUpdateSkinPartition(NiSkinInstance * skinInstance, NiSkinPartition * partition, bool gpuCopy = false, bool rebindDynamic = false, const UInt8 * vertexBlock = nullptr);

private:
	NiPointer<NiSkinPartition>	m_partition;
	NiPointer<NiSkinInstance>	m_skinInstance;
	const UInt8 * m_vertexBlock;
	bool m_copyGPU;
	bool m_rebindDynamic;
};

class BodyGenMorphData
{
public:
	SKEEFixedString	name;
	float			lower;
	float			upper;
};

class BodyGenMorphSelector : public std::vector<BodyGenMorphData>
{
public:
	UInt32 Evaluate(std::function<void(SKEEFixedString, float)> eval);
};

class BodyGenMorphs : public std::vector<BodyGenMorphSelector>
{
public:
	UInt32 Evaluate(std::function<void(SKEEFixedString, float)> eval);
};

class BodyGenTemplate : public std::vector<BodyGenMorphs>
{
public:
	UInt32 Evaluate(std::function<void(SKEEFixedString, float)> eval);
};
typedef std::shared_ptr<BodyGenTemplate> BodyGenTemplatePtr;


typedef absl::flat_hash_map<SKEEFixedString, BodyGenTemplatePtr> BodyGenTemplates;

class BodyTemplateList : public std::vector<BodyGenTemplatePtr>
{
public:
	UInt32 Evaluate(std::function<void(SKEEFixedString, float)> eval);
};

class BodyGenDataTemplates : public std::vector<BodyTemplateList>
{
public:
	UInt32 Evaluate(std::function<void(SKEEFixedString, float)> eval);
};
typedef std::shared_ptr<BodyGenDataTemplates> BodyGenDataTemplatesPtr;

typedef absl::flat_hash_map<TESNPC*, BodyGenDataTemplatesPtr> BodyGenData;

struct MorphShapeCallbackItem
{
	IBodyMorphInterface::MorphShapeCallback cb;
	uint64_t sort;

	struct Comp {
		bool operator()(const MorphShapeCallbackItem& a, const MorphShapeCallbackItem& b) const
		{
			if (a.sort == b.sort)
			{
				return a.cb < b.cb;
			}
			return a.sort > b.sort; // Descending by age
		}
	};
};

class MorphShapeCallbacks : public SafeDataHolder<absl::btree_set<MorphShapeCallbackItem, MorphShapeCallbackItem::Comp>>
{
public:
	void AddCallback(IBodyMorphInterface::MorphShapeCallback cb, skee_u64 order = 0);
	void ForEach(std::function<void(IBodyMorphInterface::MorphShapeCallback)> func);
};

class BodyMorphInterface 
	: public IBodyMorphInterface
	, public IAddonAttachmentInterface
{
public:
	virtual skee_u32 GetVersion();

	// Serialization
	void Save(SKSESerializationInterface * intfc, UInt32 kVersion);
	bool Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable);
	virtual void Revert() override;

	virtual void SetMorph(TESObjectREFR * actor, const char * morphName, const char * morphKey, float relative) override { Impl_SetMorph(actor, morphName, morphKey, relative); }
	virtual float GetMorph(TESObjectREFR * actor, const char * morphName, const char * morphKey) override { return Impl_GetMorph(actor, morphName, morphKey); }
	virtual void ClearMorph(TESObjectREFR * actor, const char * morphName, const char * morphKey) override { Impl_ClearMorph(actor, morphName, morphKey); }

	virtual float GetBodyMorphs(TESObjectREFR * actor, const char * morphName) override { return Impl_GetBodyMorphs(actor, morphName); }
	virtual void ClearBodyMorphNames(TESObjectREFR * actor, const char * morphName) override { Impl_ClearBodyMorphNames(actor, morphName); }

	virtual void VisitMorphValues(TESObjectREFR * actor, MorphValueVisitor & visitor) override
	{
		Impl_VisitMorphs(actor, [&](SKEEFixedString key, absl::flat_hash_map<StringTableItem, float> * map)
		{
			for (auto & item : *map)
			{
				visitor.Visit(actor, key.c_str(), item.first->c_str(), item.second);
			}
		});
	}

	virtual void VisitMorphs(TESObjectREFR * actor, MorphVisitor & visitor) override { Impl_VisitMorphs(actor, [&](SKEEFixedString key, absl::flat_hash_map<StringTableItem, float> * map) { visitor.Visit(actor, key.c_str()); }); }
	virtual void VisitKeys(TESObjectREFR * actor, const char * name, MorphKeyVisitor & visitor) override { Impl_VisitKeys(actor, name, [&](SKEEFixedString key, float value) { visitor.Visit(key.c_str(), value); }); }

	virtual void ClearMorphs(TESObjectREFR * actor) override { Impl_ClearMorphs(actor); }

	virtual void ApplyVertexDiff(TESObjectREFR * refr, NiAVObject * rootNode, bool erase = false) override { Impl_ApplyVertexDiff(refr, rootNode, erase); }

	virtual void ApplyBodyMorphs(TESObjectREFR * refr, bool deferUpdate = true) override { Impl_ApplyBodyMorphs(refr, deferUpdate); }
	virtual void UpdateModelWeight(TESObjectREFR * refr, bool immediate = false) override { Impl_UpdateModelWeight(refr, immediate); }

	virtual void SetCacheLimit(size_t limit) override { Impl_SetCacheLimit(limit); }
	virtual bool HasMorphs(TESObjectREFR * actor) override { return Impl_HasMorphs(actor); }
	virtual skee_u32 EvaluateBodyMorphs(TESObjectREFR * actor) override { return Impl_EvaluateBodyMorphs(actor); }

	virtual bool HasBodyMorph(TESObjectREFR * actor, const char * morphName, const char * morphKey) override { return Impl_HasBodyMorph(actor, morphName, morphKey); }
	virtual bool HasBodyMorphName(TESObjectREFR * actor, const char * morphName) override { return Impl_HasBodyMorphName(actor, morphName); }
	virtual bool HasBodyMorphKey(TESObjectREFR * actor, const char * morphKey) override { return Impl_HasBodyMorphKey(actor, morphKey); }
	virtual void ClearBodyMorphKeys(TESObjectREFR * actor, const char * morphKey) override { Impl_ClearBodyMorphKeys(actor, morphKey); }
	virtual void VisitStrings(StringVisitor & visitor) override { Impl_VisitStrings([&visitor](SKEEFixedString key) { visitor.Visit(key.c_str()); }); }
	virtual void VisitActors(ActorVisitor & visitor) override { Impl_VisitActors([&visitor](TESObjectREFR* actor) { visitor.Visit(actor); }); }
	virtual std::vector<SKEEFixedString> GetCachedMorphNames();

	virtual size_t ClearMorphCache() override;

	virtual void AddMorphShapeCallback(IBodyMorphInterface::MorphShapeCallback cb, skee_u64 order = 0) override;

	void LoadMods();
	void PrintDiagnostics();

	void ForEachMorphShapeCallback(std::function<void(IBodyMorphInterface::MorphShapeCallback)> func);
	MorphFactors GetMorphFactors(TESObjectREFR * actor);

private:
	void GetFilteredNPCList(std::vector<TESNPC*> activeNPCs[], const ModInfo * modInfo, UInt32 gender, TESRace * raceFilter, absl::flat_hash_set<TESFaction*> factionList);
	bool IsNPCInFactions(TESNPC* npc, absl::flat_hash_set<TESFaction*> factionList);

	void Impl_SetMorph(TESObjectREFR * actor, SKEEFixedString morphName, SKEEFixedString morphKey, float relative);
	float Impl_GetMorph(TESObjectREFR * actor, SKEEFixedString morphName, SKEEFixedString morphKey);
	void Impl_ClearMorph(TESObjectREFR * actor, SKEEFixedString morphName, SKEEFixedString morphKey);

	float Impl_GetBodyMorphs(TESObjectREFR * actor, SKEEFixedString morphName);
	void Impl_ClearBodyMorphNames(TESObjectREFR * actor, SKEEFixedString morphName);

	void Impl_VisitMorphs(TESObjectREFR * actor, std::function<void(SKEEFixedString, absl::flat_hash_map<StringTableItem, float> *)> functor);
	void Impl_VisitKeys(TESObjectREFR * actor, SKEEFixedString name, std::function<void(SKEEFixedString, float)> functor);

	void Impl_ClearMorphs(TESObjectREFR * actor);

	void Impl_ApplyVertexDiff(TESObjectREFR * refr, NiAVObject * rootNode, bool erase = false);

	void Impl_ApplyBodyMorphs(TESObjectREFR * refr, bool deferUpdate = true);
	void Impl_UpdateModelWeight(TESObjectREFR * refr, bool immediate = false);

	void Impl_SetCacheLimit(size_t limit);
	bool Impl_HasMorphs(TESObjectREFR * actor);

	bool Impl_ReadBodyMorphs(SKEEFixedString filePath);
	bool Impl_ReadBodyMorphTemplates(SKEEFixedString filePath);
	UInt32 Impl_EvaluateBodyMorphs(TESObjectREFR * actor);

	bool Impl_HasBodyMorph(TESObjectREFR * actor, SKEEFixedString morphName, SKEEFixedString morphKey);
	bool Impl_HasBodyMorphName(TESObjectREFR * actor, SKEEFixedString morphName);
	bool Impl_HasBodyMorphKey(TESObjectREFR * actor, SKEEFixedString morphKey);
	void Impl_ClearBodyMorphKeys(TESObjectREFR * actor, SKEEFixedString morphKey);
	void Impl_VisitStrings(std::function<void(SKEEFixedString)> functor);
	void Impl_VisitActors(std::function<void(TESObjectREFR*)> functor);

private:
	ActorMorphs	actorMorphs;
	MorphCache	morphCache;
	BodyGenTemplates bodyGenTemplates;
	BodyGenData	bodyGenData[2];

	MorphShapeCallbacks shapeCallbacks;

	friend class NIOVTaskUpdateMorph;
	friend class NIOVTaskUpdateModelWeight;
	friend class PresetInterface;

	// Inherited via IAddonAttachmentInterface
	virtual void OnAttach(TESObjectREFR * refr, TESObjectARMO * armor, TESObjectARMA * addon, NiAVObject * object, bool isFirstPerson, NiNode * skeleton, NiNode * root) override;
};
