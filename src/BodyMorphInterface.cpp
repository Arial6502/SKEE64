#include "BodyMorphInterface.h"
#include "OverlayInterface.h"
#include "AttachmentInterface.h"
#include "ActorUpdateManager.h"
#include "ShaderUtilities.h"
#include "StringTable.h"
#include "Utilities.h"

#include "FileUtils.h"
#include "NifUtils.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iterator>
#include <map>
#include <thread>
#include <unordered_set>
#include <string>
#include <limits>
#include <cctype>
#include <random>
#include <immintrin.h>
#include <ppl.h>

#include "skse64/PluginAPI.h"
#include "skse64/GameReferences.h"
#include "skse64/GameStreams.h"
#include "skse64/GameRTTI.h"
#include "skse64/GameObjects.h"
#include "skse64/GameForms.h"
#include "skse64/GameData.h"
#include "skse64/GameExtraData.h"

#include "skse64/NiRenderer.h"
#include "skse64/NiNodes.h"
#include "skse64/NiGeometry.h"
#include "skse64/NiExtraData.h"
#include "skse64/NiRTTI.h"

#include "Morpher.h"
#include "MainThreadTasks.h"

extern ActorUpdateManager				g_actorUpdateManager;
extern BodyMorphInterface				g_bodyMorphInterface;
extern OverlayInterface					g_overlayInterface;
extern StringTable						g_stringTable;
extern SKSETaskInterface				* g_task;
extern bool								g_parallelMorphing;
extern UInt16							g_bodyMorphMode;
extern bool								g_enableBodyGen;
extern bool								g_enableBodyMorph;
extern bool								g_enableBodyNormalRecalculate;
extern bool								g_bodyMorphGPUCopy;
extern bool								g_bodyMorphRebind;
extern bool								g_logMorphTiming;
extern bool								g_morphCollision;
extern float							g_morphCollisionDistance;
extern float							g_morphCollisionClearance;
extern bool								g_morphConform;
extern bool								g_morphConformWithoutTRI;
extern float							g_morphConformDistance;
extern float							g_morphConformMismatch;
extern bool								g_morphFitDiskCache;
extern UInt64							g_morphFitDiskLimit;
extern UInt64							g_morphFitMemoryLimit;

skee_u32 BodyMorphInterface::GetVersion()
{
	return kCurrentPluginVersion;
}

void BodyMorphInterface::PrintDiagnostics()
{
	Console_Print("BodyMorphInterface Diagnostics:");
	actorMorphs.Lock();
	Console_Print("\t%llu actors morphed", actorMorphs.m_data.size());
	for (auto& entry : actorMorphs.m_data)
	{
		TESForm* form = LookupFormByID(entry.first);
		TESObjectREFR* refr = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr;
		_MESSAGE("Reference: %08X (%s) (%lld keys)", entry.first, refr ? CALL_MEMBER_FN(refr, GetReferenceName)() : "", entry.second.size());
		for (auto& item : entry.second)
		{
			_MESSAGE("\tMorph: %s (%lld morphs)", item.first ? item.first->c_str() : "", item.second.size());
			for (auto& morph : item.second)
			{
				_MESSAGE("\t\tKey: %s Value: %f", morph.first ? morph.first->c_str() : "", morph.second);
			}
		}
	}
	actorMorphs.Release();
	morphCache.Lock();
	Console_Print("\t%llu bytes cached", morphCache.totalMemory);
	Console_Print("\t%llu files cached", morphCache.m_data.size());
	morphCache.Release();
	Console_Print("\t%llu BodyGen templates loaded", bodyGenTemplates.size());
	Console_Print("\t%llu BodyGen male candidates", bodyGenData[0].size());
	Console_Print("\t%llu BodyGen female candidates", bodyGenData[1].size());

	morphCache.ForEachMorphFile([&](const SKEEFixedString& filePath, const MorphFileCache& morphFile)
	{
		_MESSAGE("File: %s (%llu bytes)", filePath.c_str(), morphFile.GetByteSize());
		morphFile.ForEachShape([&](const SKEEFixedString& shapeName, const BodyMorphMap& morphMap)
		{
			_MESSAGE("\tShape: %s (%llu total morphs)", shapeName.c_str(), morphMap.size());
			morphMap.ForEachMorph([&](const SKEEFixedString& morphName, const auto vertexData)
			{
				_MESSAGE("\t\tMorph: %s (%llu verts %llu uv)", morphName.c_str(), vertexData.first ? vertexData.first->GetSize() : 0LL, vertexData.second ? vertexData.second->GetSize() : 0LL);
			});
		});
	});
}

std::vector<SKEEFixedString> BodyMorphInterface::GetCachedMorphNames()
{
	std::vector<SKEEFixedString> morphList;
	absl::flat_hash_set<SKEEFixedString> morphNames;
	morphCache.ForEachMorphFile([&](const SKEEFixedString& filePath, const MorphFileCache& morphFile)
	{
		morphFile.ForEachShape([&](const SKEEFixedString& shapeName, const BodyMorphMap& morphMap)
		{
			morphMap.ForEachMorph([&](const SKEEFixedString& morphName, const auto vertexData)
			{
				if (!morphNames.count(morphName))
				{
					morphList.push_back(morphName);
					morphNames.emplace(morphName);
				}
			});
		});
	});
	return morphList;
}

size_t BodyMorphInterface::ClearMorphCache()
{
	return morphCache.Clear();
}

void BodyMorphInterface::LoadMods()
{
	DataHandler * dataHandler = DataHandler::GetSingleton();
	if (dataHandler)
	{
		std::string fixedPath = "Meshes\\" + std::string(MORPH_MOD_DIRECTORY);

		ForEachMod([&](ModInfo * modInfo)
		{
			std::string templatesPath = fixedPath + std::string(modInfo->name) + "\\templates.ini";
			Impl_ReadBodyMorphTemplates(templatesPath.c_str());
		});

		ForEachMod([&](ModInfo * modInfo)
		{
			std::string morphsPath = fixedPath + std::string(modInfo->name) + "\\morphs.ini";
			Impl_ReadBodyMorphs(morphsPath.c_str());
		});
	}
}

void BodyMorphInterface::Revert()
{
	SimpleLocker locker(&actorMorphs.m_lock);
	actorMorphs.m_data.clear();
}

void BodyMorphInterface::Impl_SetMorph(TESObjectREFR * actor, SKEEFixedString morphName, SKEEFixedString morphKey, float relative)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	actorMorphs.m_data[actor->formID][g_stringTable.GetString(morphName)][g_stringTable.GetString(morphKey)] = relative;
}

float BodyMorphInterface::Impl_GetMorph(TESObjectREFR * actor, SKEEFixedString morphName, SKEEFixedString morphKey)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if(it != actorMorphs.m_data.end())
	{
		auto mit = it->second.find(g_stringTable.GetString(morphName));
		if (mit != it->second.end())
		{
			auto kit = mit->second.find(g_stringTable.GetString(morphKey));
			if (kit != mit->second.end())
			{
				return kit->second;
			}
		}
	}

	return 0.0;
}

void BodyMorphInterface::Impl_ClearMorph(TESObjectREFR * actor, SKEEFixedString morphName, SKEEFixedString morphKey)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		auto mit = it->second.find(g_stringTable.GetString(morphName));
		if (mit != it->second.end())
		{
			auto kit = mit->second.find(g_stringTable.GetString(morphKey));
			if (kit != mit->second.end())
			{
				mit->second.erase(kit);
			}
		}
	}
}

bool BodyMorphInterface::Impl_HasBodyMorph(TESObjectREFR * actor, SKEEFixedString morphName, SKEEFixedString morphKey)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		auto kit = it->second.find(g_stringTable.GetString(morphName));
		if (kit != it->second.end())
		{
			auto mit = kit->second.find(g_stringTable.GetString(morphKey));
			if(mit != kit->second.end())
				return true;
		}
	}

	return false;
}

static float CombineMorphValues(const absl::flat_hash_map<StringTableItem, float> & values)
{
	float morphSum = 0;
	for (auto & morph : values)
	{
		if (g_bodyMorphMode == 2 && morph.second > morphSum)
		{
			morphSum = morph.second;
		}
		else
		{
			morphSum += morph.second;
		}
	}

	if (g_bodyMorphMode == 1)
	{
		morphSum /= values.size();
	}

	return morphSum;
}

float BodyMorphInterface::Impl_GetBodyMorphs(TESObjectREFR * actor, SKEEFixedString morphName)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		auto mit = it->second.find(g_stringTable.GetString(morphName));
		if (mit != it->second.end())
		{
			return CombineMorphValues(mit->second);
		}
	}

	return 0.0;
}

MorphFactors BodyMorphInterface::GetMorphFactors(TESObjectREFR * actor)
{
	MorphFactors factors;

	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		factors.reserve(it->second.size());
		for (auto & morph : it->second)
		{
			float factor = CombineMorphValues(morph.second);
			if (factor != 0.0f)
				factors.emplace_back(morph.first, factor);
		}
	}

	return factors;
}

bool BodyMorphInterface::Impl_HasBodyMorphKey(TESObjectREFR * actor, SKEEFixedString morphKey)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		for (auto& mit : it->second)
		{
			auto kit = mit.second.find(g_stringTable.GetString(morphKey));
			if (kit != mit.second.end())
			{
				return true;
			}
		}
	}

	return false;
}

void BodyMorphInterface::Impl_ClearBodyMorphKeys(TESObjectREFR * actor, SKEEFixedString morphKey)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		for (auto& mit : it->second)
		{
			auto kit = mit.second.find(g_stringTable.GetString(morphKey));
			if (kit != mit.second.end())
			{
				mit.second.erase(kit);
			}
		}
	}
}

bool BodyMorphInterface::Impl_HasBodyMorphName(TESObjectREFR * actor, SKEEFixedString morphName)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		auto kit = it->second.find(g_stringTable.GetString(morphName));
		if (kit != it->second.end())
		{
			return true;
		}
	}

	return false;
}

void BodyMorphInterface::Impl_ClearBodyMorphNames(TESObjectREFR * actor, SKEEFixedString morphName)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		auto mit = it->second.find(g_stringTable.GetString(morphName));
		if (mit != it->second.end())
		{
			mit->second.clear();
		}
	}
}

void BodyMorphInterface::Impl_ClearMorphs(TESObjectREFR * actor)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if(it != actorMorphs.m_data.end())
	{
		actorMorphs.m_data.erase(it);
	}
}

bool BodyMorphInterface::Impl_HasMorphs(TESObjectREFR * actor)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		return true;
	}

	return false;
}

void TriShapeFullVertexData::ApplyMorphRaw(UInt16 vertCount, void * data, float factor)
{
	NiPoint3 * vertices = static_cast<NiPoint3*>(data);
	if (!vertices)
		return;

	if (m_maxIndex < vertCount)
	{
		for (const auto & vert : m_vertexDeltas)
		{
			UInt16 vertexIndex = vert.index;
			const DirectX::XMVECTOR * vertexDiff = &vert.delta;

			vertices[vertexIndex].x += vertexDiff->m128_f32[0] * factor;
			vertices[vertexIndex].y += vertexDiff->m128_f32[1] * factor;
			vertices[vertexIndex].z += vertexDiff->m128_f32[2] * factor;

		}
	}
	else
	{
		_ERROR("%s - Failed to apply morphs to geometry - morphs largest index is %d mesh vertex size is %d", __FUNCTION__, m_maxIndex, (UInt32)vertCount);
	}
}

void TriShapeFullVertexData::ApplyMorph(UInt16 vertexCount, Layout * vertexData, float factor)
{
	UInt32 offset = NiSkinPartition::GetVertexAttributeOffset(vertexData->vertexDesc, VertexAttribute::VA_POSITION);
	UInt32 vertexSize = NiSkinPartition::GetVertexSize(vertexData->vertexDesc);

	if (m_maxIndex < vertexCount)
	{
		for (const auto & vert : m_vertexDeltas)
		{
			DirectX::XMFLOAT4 * position = reinterpret_cast<DirectX::XMFLOAT4*>(&vertexData->vertexData[vertexSize * vert.index + offset]);
			DirectX::XMStoreFloat4(position, DirectX::XMVectorAdd(DirectX::XMLoadFloat4(position), DirectX::XMVectorScale(vert.delta, factor)));
		}
	}
	else
	{
		_ERROR("%s - Failed to apply morphs to geometry - morphs largest index is %d mesh vertex size is %d", __FUNCTION__, m_maxIndex, (UInt32)vertexCount);
	}
}

void TriShapePackedVertexData::ApplyMorphRaw(UInt16 vertCount, void * data, float factor)
{
	NiPoint3 * vertices = static_cast<NiPoint3*>(data);
	if (!vertices)
		return;

	if (m_maxIndex < vertCount)
	{
		for (const auto & vert : m_vertexDeltas)
		{
			UInt32 vertexIndex = vert.index;
			vertices[vertexIndex].x += (float)vert.x * m_multiplier * factor;
			vertices[vertexIndex].y += (float)vert.y * m_multiplier * factor;
			vertices[vertexIndex].z += (float)vert.z * m_multiplier * factor;
		}
	}
	else
	{
		_ERROR("%s - Failed to apply morphs to geometry - morphs largest index is %d mesh vertex size is %d", __FUNCTION__, m_maxIndex, (UInt32)vertCount);
	}
}

void TriShapePackedVertexData::ApplyMorph(UInt16 vertexCount, Layout * vertexData, float factor)
{
	UInt32 vertexSize = NiSkinPartition::GetVertexSize(vertexData->vertexDesc);
	UInt32 offset = NiSkinPartition::GetVertexAttributeOffset(vertexData->vertexDesc, VertexAttribute::VA_POSITION);

	if (m_maxIndex < vertexCount)
	{
		const __m128 scale = _mm_set1_ps(factor * m_multiplier);
		for (const auto & vert : m_vertexDeltas)
		{
			float * position = reinterpret_cast<float*>(&vertexData->vertexData[vertexSize * vert.index + offset]);

			// Widens index, x, y, z and shifts the index out, so the fourth float (bitangent x) gets nothing added
			const __m128i widened = _mm_cvtepi16_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(&vert)));
			const __m128 delta = _mm_cvtepi32_ps(_mm_srli_si128(widened, 4));
			_mm_storeu_ps(position, _mm_add_ps(_mm_loadu_ps(position), _mm_mul_ps(delta, scale)));
		}
	}
	else
	{
		_ERROR("%s - Failed to apply morphs to geometry - morphs largest index is %d mesh vertex size is %d", __FUNCTION__, m_maxIndex, (UInt32)vertexCount);
	}
}

void TriShapePackedUVData::ApplyMorphRaw(UInt16 vertCount, void * data, float factor)
{
	UVCoord * deltas = static_cast<UVCoord*>(data);
	if (!deltas)
		return;

	if (m_maxIndex < vertCount)
	{
		for (const auto & delta : m_uvDeltas)
		{
			UInt32 vertexIndex = delta.index;
			deltas[vertexIndex].u += (float)delta.u * m_multiplier * factor;
			deltas[vertexIndex].v += (float)delta.v * m_multiplier * factor;
		}
	}
	else
	{
		_ERROR("%s - Failed to apply morphs to geometry - morphs largest index is %d mesh vertex size is %d", __FUNCTION__, m_maxIndex, (UInt32)vertCount);
	}
}

void TriShapePackedUVData::ApplyMorph(UInt16 vertexCount, Layout * vertexData, float factor)
{
	VertexFlags flags = NiSkinPartition::GetVertexFlags(vertexData->vertexDesc);
	if ((flags & VF_UV))
	{
		UInt32 vertexSize = NiSkinPartition::GetVertexSize(vertexData->vertexDesc);
		UInt32 offset = NiSkinPartition::GetVertexAttributeOffset(vertexData->vertexDesc, VertexAttribute::VA_TEXCOORD0);
		if (m_maxIndex < vertexCount)
		{
			for (const auto & delta : m_uvDeltas)
			{
				UVCoord * texCoord = reinterpret_cast<UVCoord*>(&vertexData->vertexData[vertexSize * delta.index + offset]);
				texCoord->u += (float)delta.u * m_multiplier * factor;
				texCoord->v += (float)delta.v * m_multiplier * factor;
			}
		}
		else
		{
			_ERROR("%s - Failed to apply morphs to geometry - morphs largest index is %d mesh vertex size is %d", __FUNCTION__, m_maxIndex, (UInt32)vertexCount);
		}
	}
}

void BodyMorphMap::ForEachMorph(std::function<void(const SKEEFixedString&, const std::pair<TriShapeVertexDataPtr, TriShapeVertexDataPtr>&)> functor) const
{
	for (auto& morph : *this)
	{
		functor(morph.first, morph.second);
	}
}

#include <fstream>
#include <regex>
#include <d3d11.h>
#include <d3d11_4.h>

const MorphKernel::Topology * BodyMorphMap::GetTopology(const UInt8 * vertices, UInt32 vertexCount, const MorphKernel::VertexLayout & layout, const std::vector<MorphKernel::TriangleRun> & triangles, const std::vector<UInt16> & lockedVertices) const
{
	std::lock_guard<std::mutex> guard(m_topologies->lock);

	for (const auto & mesh : m_topologies->meshes)
	{
		if (mesh->Matches(vertices, vertexCount, layout, triangles, lockedVertices))
			return mesh.get();
	}

	m_topologies->meshes.push_back(std::make_unique<MorphKernel::Topology>(vertices, vertexCount, layout, triangles, lockedVertices));
	return m_topologies->meshes.back().get();
}

static bool FitsArmor()
{
	return g_morphCollision || g_morphConform;
}

// The sliders of a TRI by name for the fit threads. They hold its deltas, so a TRI dropped from the cache meanwhile stays
// alive for them.
static MorphCollision::FitTRI FitTRIOf(const BodyMorphMap * morphs, UInt32 vertexCount)
{
	MorphCollision::FitTRI tri;
	std::vector<TriShapeVertexDataPtr> deltas;
	if (morphs)
	{
		for (const auto & [name, data] : *morphs)
		{
			if (data.first)
			{
				tri.names.push_back(name.c_str());
				deltas.push_back(data.first);
			}
		}
	}

	tri.delta = [deltas = std::move(deltas), vertexCount](uint32_t slider, float * out)
	{
		deltas[slider]->ApplyMorphRaw(UInt16(vertexCount), out, 1.0f);
	};
	return tri;
}

// Which sliders a TRI has and how many vertices each moves, in whatever order the map holds them. Part of what an overlay
// is kept under.
static uint64_t TRISignature(const BodyMorphMap * morphs)
{
	uint64_t signature = 0;
	if (!morphs)
		return signature;

	for (const auto & [name, data] : *morphs)
	{
		uint64_t entry = 0xCBF29CE484222325ull;
		for (const char * c = name.c_str(); *c; ++c)
			entry = (entry ^ UInt8(std::tolower(UInt8(*c)))) * 0x100000001B3ull;

		const uint64_t size = data.first ? data.first->GetSize() + 1 : 0;
		signature += MorphCollision::Hash(&size, sizeof(size), entry);
	}
	return signature;
}

// A skin shape of the actor, which armor on the same skeleton is fitted to
struct CollisionTarget
{
	struct Snapshot
	{
		std::once_flag									once;
		std::shared_ptr<const MorphCollision::FitSkin>	skin;
	};

	NiPointer<BSGeometry>					geometry;
	NiPointer<NiSkinPartition>				partition;
	NiAVObject *							skeleton;
	const NiTransform *						rootParentToSkin;
	MorphCollision::Mesh					rest;
	MorphCollision::Mesh					morphed;	// As this update leaves it, armor is pushed out of it
	std::vector<MorphKernel::TriangleRun>	triangles;
	const BodyMorphMap *					morphs;		// Null for skin no TRI covers, which never moves
	uint64_t								key;		// The rest shape and the TRI, part of what an overlay is kept under
	std::shared_ptr<Snapshot>				snapshot = std::make_shared<Snapshot>();

	// Copies for the fit threads, made once in a run and only when some overlay is missing sliders
	std::shared_ptr<const MorphCollision::FitSkin> Skin() const
	{
		std::call_once(snapshot->once, [this]()
		{
			auto skin = std::make_shared<MorphCollision::FitSkin>();
			skin->rest = MorphCollision::Positions(rest);
			for (const auto & run : triangles)
				skin->triangles.insert(skin->triangles.end(), run.indices, run.indices + run.count * 3);
			skin->tri = FitTRIOf(morphs, rest.vertexCount);
			snapshot->skin = std::move(skin);
		});
		return snapshot->skin;
	}
};

static UInt32 ShaderType(BSGeometry * geometry)
{
	BSShaderProperty * shaderProperty = niptr_cast<BSShaderProperty>(geometry->m_spEffectState);
	if (!shaderProperty || !ni_is_type(shaderProperty->GetRTTI(), BSLightingShaderProperty))
		return UINT32_MAX;

	BSLightingShaderMaterial * material = (BSLightingShaderMaterial*)shaderProperty->material;
	return material ? material->GetShaderType() : UINT32_MAX;
}

static bool IsSkin(BSGeometry * geometry)
{
	return ShaderType(geometry) == BSLightingShaderMaterial::kShaderType_FaceGenRGBTint;
}

// Hair and eyes sit close to the skin without being worn on it
static bool ConformsWithoutTRI(BSGeometry * geometry)
{
	switch (ShaderType(geometry))
	{
	case BSLightingShaderMaterial::kShaderType_FaceGen:
	case BSLightingShaderMaterial::kShaderType_FaceGenRGBTint:
	case BSLightingShaderMaterial::kShaderType_HairTint:
	case BSLightingShaderMaterial::kShaderType_Eye:
		return false;
	default:
		return geometry->m_spSkinInstance && !ni_cast(geometry, BSDynamicTriShape);
	}
}

static bool ContainsSkin(NiAVObject * object)
{
	return VisitGeometry(object, [](BSGeometry * geometry) { return IsSkin(geometry); });
}

// Fitting reads positions straight out of the partition's vertex block. Skinned partitions always hold float positions,
// without VF_FULLPREC being set, while dynamic shapes keep theirs elsewhere.
static bool HasPartitionPositions(BSGeometry * geometry)
{
	return ni_cast(geometry, BSTriShape) && !ni_cast(geometry, BSDynamicTriShape);
}

static void AddCollisionTarget(std::vector<CollisionTarget> & targets, BSGeometry * geometry, const UInt8 * morphed, const BodyMorphMap * morphs)
{
	NiPointer<NiSkinInstance> skinInstance = niptr_cast<NiSkinInstance>(geometry->m_spSkinInstance);
	if (!skinInstance || !skinInstance->m_spSkinData)
		return;

	NiPointer<NiSkinPartition> partition = niptr_cast<NiSkinPartition>(skinInstance->m_spSkinPartition);
	if (!partition || !partition->m_uiPartitions || !HasPartitionPositions(geometry))
		return;

	auto & firstPartition = partition->m_pkPartitions[0];
	const UInt32 stride = NiSkinPartition::GetVertexSize(firstPartition.vertexDesc);
	const UInt32 vertexCount = partition->vertexCount;

	NiBinaryExtraData * bodyData = (NiBinaryExtraData*)NifUtils::GetExtraData(geometry, "SHAPEDATA");
	const UInt8 * rest = bodyData && bodyData->m_size == stride * vertexCount ? reinterpret_cast<const UInt8*>(bodyData->m_data) : firstPartition.shapeData->m_RawVertexData;
	const MorphCollision::Mesh restMesh = { rest, vertexCount, stride };

	CollisionTarget target = { geometry, partition, skinInstance->m_pkRootParent, &skinInstance->m_spSkinData->m_kRootParentToSkin,
		restMesh, { morphed ? morphed : firstPartition.shapeData->m_RawVertexData, vertexCount, stride } };
	target.morphs = morphs;
	const uint64_t signature = TRISignature(morphs);
	target.key = MorphCollision::Hash(&signature, sizeof(signature), MorphCollision::Fingerprint(restMesh));
	for (UInt32 p = 0; p < partition->m_uiPartitions; ++p)
	{
		target.triangles.push_back({ partition->m_pkPartitions[p].m_pusTriList, partition->m_pkPartitions[p].m_usTriangles });
		target.key = MorphCollision::Hash(&partition->m_pkPartitions[p].m_usTriangles, sizeof(UInt16), target.key);
	}

	targets.push_back(std::move(target));
}

// Exported meshes often carry -0 where the body has 0, so the transforms can't be compared bytewise
static bool SameTransform(const NiTransform & a, const NiTransform & b)
{
	const float * x = reinterpret_cast<const float*>(&a);
	const float * y = reinterpret_cast<const float*>(&b);
	for (size_t i = 0; i < sizeof(NiTransform) / sizeof(float); ++i)
	{
		if (std::fabs(x[i] - y[i]) > 1e-4f)
			return false;
	}
	return true;
}

// Every shape of armor gets an overlay per skin it sits on, generated morphs that fill in sliders its TRI lacks and correct
// ones it has out of date, made on threads of their own below the game's priority and apart from the morph workers.
// An overlay only looks at sliders actors use and grows as they use more. It doesn't depend on the sliders' values, so
// it's applied in every update like a TRI, and kept in memory and on disk for later sessions.
class OverlayService
{
public:
	static OverlayService & Get()
	{
		static OverlayService * service = new OverlayService(); // Never destroyed, its threads run until the game exits
		return *service;
	}

	// The overlay kept under key, from memory or from the disk, null if none was made yet
	std::shared_ptr<const MorphCollision::FitMorphs> Find(uint64_t key, uint32_t vertexCount)
	{
		std::unique_lock<std::mutex> lock(m_lock);
		if (auto it = m_overlays.find(key); it != m_overlays.end())
		{
			it->second.used = ++m_clock;
			return it->second.overlay;
		}

		if (!m_onDisk.count(key))
			return nullptr;
		lock.unlock();

		auto overlay = MorphCollision::FitMorphs::Deserialize(MorphCollision::ReadFile(File(key)), vertexCount);

		lock.lock();
		if (!overlay)
		{
			m_onDisk.erase(key);
			return nullptr;
		}

		Store(key, overlay);
		return overlay;
	}

	// The shape waits on key from now. True when the caller should submit the sliders it's missing, false when some are
	// already on their way, the shape asks again after.
	bool Claim(uint64_t key, UInt32 formId, uint64_t shape)
	{
		std::lock_guard<std::mutex> guard(m_lock);
		const Waiter waiter = { formId, shape };
		auto [latest, fresh] = m_latest.try_emplace(shape, key);
		if (!fresh)
		{
			if (latest->second == key)
				return !m_jobs.count(key) && m_jobs.insert(key).second;
			Unwait(latest->second, waiter);
			latest->second = key;
		}

		m_waiters[key].push_back(waiter);
		++m_pending[formId];
		return m_jobs.insert(key).second;
	}

	void Submit(uint64_t key, MorphCollision::FitInput && input)
	{
		{
			std::lock_guard<std::mutex> guard(m_lock);
			m_queue.push_back({ key, std::move(input) });
		}
		m_ready.notify_one();
	}

	size_t ByteSize()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		return m_bytes + m_bindings.ByteSize();
	}

	// Refits held back since last asked, a few on a busy load are expected
	UInt32 TakeHeldBack()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		return std::exchange(m_heldBack, 0);
	}

	// What waits to be made is still made
	void Clear()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		m_overlays.clear();
		m_bytes = 0;
		m_bindings.Clear();
	}

private:
	struct Waiter
	{
		UInt32		formId;
		uint64_t	shape;

		bool operator==(const Waiter & rhs) const = default;
	};

	struct Job
	{
		uint64_t					key;
		MorphCollision::FitInput	input;
	};

	struct Entry
	{
		std::shared_ptr<const MorphCollision::FitMorphs>	overlay;
		uint64_t											used = 0;
	};

	OverlayService()
	{
		m_settings = { g_morphConform, g_morphConformMismatch, 0.01f };

		// Next to the plugin, in a folder named after it
		wchar_t runtime[MAX_PATH];
		const DWORD length = GetModuleFileNameW(nullptr, runtime, MAX_PATH);
		if (g_morphFitDiskCache && length && length < MAX_PATH)
			m_folder = std::filesystem::path(runtime).parent_path() / L"Data" / L"SKSE" / L"Plugins" / L"SKEE" / L"Overlays";

		m_bindings.Configure({ g_morphCollision ? g_morphCollisionDistance : 0.0f, g_morphConform ? g_morphConformDistance : 0.0f }, g_morphFitMemoryLimit);

		const unsigned threads = (std::clamp)(std::thread::hardware_concurrency() / 4, 1u, 4u);
		for (unsigned i = 0; i < threads; ++i)
		{
			std::thread([this]()
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
				Work();
			}).detach();
		}

		if (!m_folder.empty())
		{
			std::thread([this]()
			{
				SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
				Sweep();
			}).detach();
		}
	}

	std::filesystem::path File(uint64_t key) const
	{
		return m_folder / MorphCollision::FileName(key);
	}

	// Drops the oldest overlays past the disk limit, then lists the ones earlier sessions left
	void Sweep()
	{
		std::error_code error;
		std::vector<std::tuple<std::filesystem::file_time_type, uintmax_t, std::filesystem::path>> files;
		uintmax_t total = 0;
		for (std::filesystem::directory_iterator it(m_folder, error), end; !error && it != end; it.increment(error))
		{
			if (it->path().extension() == L".tmp")
			{
				std::filesystem::remove(it->path(), error);
				continue;
			}

			const uintmax_t size = it->file_size(error);
			const auto time = it->last_write_time(error);
			if (!error)
			{
				files.emplace_back(time, size, it->path());
				total += size;
			}
		}

		std::sort(files.begin(), files.end());
		std::vector<uint64_t> keys;
		for (const auto & [time, size, path] : files)
		{
			if (total > g_morphFitDiskLimit && std::filesystem::remove(path, error))
			{
				total -= size;
				continue;
			}

			const std::wstring stem = path.stem().wstring();
			if (path.extension() == L".bin" && stem.size() == 16)
				keys.push_back(std::wcstoull(stem.c_str(), nullptr, 16));
		}

		std::lock_guard<std::mutex> guard(m_lock);
		m_onDisk.insert(keys.begin(), keys.end());
	}

	void Work()
	{
		for (;;)
		{
			Job job;
			std::shared_ptr<const MorphCollision::FitMorphs> base;
			bool wanted;
			{
				std::unique_lock<std::mutex> lock(m_lock);
				m_ready.wait(lock, [this]() { return !m_queue.empty(); });
				job = std::move(m_queue.front());
				m_queue.pop_front();

				auto it = m_waiters.find(job.key);
				wanted = it != m_waiters.end() && !it->second.empty();
				if (auto stored = m_overlays.find(job.key); stored != m_overlays.end())
					base = stored->second.overlay;
			}

			const uint32_t vertexCount = uint32_t(job.input.armorRest.size() / 3);
			if (wanted && !base && !m_folder.empty())
				base = MorphCollision::FitMorphs::Deserialize(MorphCollision::ReadFile(File(job.key)), vertexCount);

			std::shared_ptr<const MorphCollision::FitMorphs> overlay;
			if (wanted)
			{
				// Sliders that failed are taken as looked at with nothing to add, so they aren't asked for in a loop
				MorphCollision::FitMorphs added;
				try
				{
					added = MorphCollision::Fit(job.input, m_bindings, m_settings);
				}
				catch (...)
				{
					added = {};
					added.vertexCount = vertexCount;
					added.looked = job.input.wanted;
					std::sort(added.looked.begin(), added.looked.end(), [](const std::string & a, const std::string & b) { return MorphCollision::CompareNames(a.c_str(), b.c_str()) < 0; });
					added.looked.erase(std::unique(added.looked.begin(), added.looked.end(), [](const std::string & a, const std::string & b) { return MorphCollision::CompareNames(a.c_str(), b.c_str()) == 0; }), added.looked.end());
					if (base)
						added.skins = base->skins;
				}

				auto merged = std::make_shared<MorphCollision::FitMorphs>(base ? MorphCollision::FitMorphs::Merge(*base, std::move(added)) : std::move(added));
				if (!m_folder.empty() && MorphCollision::WriteFile(File(job.key), merged->Serialize()))
				{
					std::lock_guard<std::mutex> guard(m_lock);
					m_onDisk.insert(job.key);
				}
				overlay = std::move(merged);
			}
			job.input = {};

			std::vector<UInt32> refits;
			{
				std::lock_guard<std::mutex> guard(m_lock);
				m_jobs.erase(job.key);
				if (overlay)
					Store(job.key, overlay);

				std::vector<Waiter> waiters;
				if (auto it = m_waiters.find(job.key); it != m_waiters.end())
				{
					waiters = std::move(it->second);
					m_waiters.erase(it);
				}

				for (const auto & waiter : waiters)
				{
					if (auto latest = m_latest.find(waiter.shape); latest != m_latest.end() && latest->second == job.key)
						m_latest.erase(latest);
					if (overlay)
						m_refit.insert(waiter.formId);
					if (Done(waiter.formId) && Allowed(waiter.formId))
						refits.push_back(waiter.formId);
				}
			}

			for (UInt32 formId : refits)
				Refit(formId);
		}
	}

	// Under the lock. True when the actor waits on nothing more and has overlays to show.
	bool Done(UInt32 formId)
	{
		auto pending = m_pending.find(formId);
		if (pending == m_pending.end() || --pending->second > 0)
			return false;

		m_pending.erase(pending);
		return m_refit.erase(formId) > 0;
	}

	// Under the lock
	void Unwait(uint64_t key, const Waiter & waiter)
	{
		auto it = m_waiters.find(key);
		if (it == m_waiters.end())
			return;

		auto & waiters = it->second;
		auto found = std::find(waiters.begin(), waiters.end(), waiter);
		if (found == waiters.end())
			return;

		waiters.erase(found);

		// Nobody waits on it anymore, its copies are let go of now rather than when a thread gets to it
		if (waiters.empty())
		{
			m_waiters.erase(it);
			auto queued = std::find_if(m_queue.begin(), m_queue.end(), [key](const Job & job) { return job.key == key; });
			if (queued != m_queue.end())
			{
				m_queue.erase(queued);
				m_jobs.erase(key);
			}
		}

		if (Done(waiter.formId) && Allowed(waiter.formId))
			Refit(waiter.formId);
	}

	// Under the lock. Overlays that keep growing would morph the actor again after each, so an actor gets a few refits in
	// a short while and then waits.
	bool Allowed(UInt32 formId)
	{
		const ULONGLONG now = GetTickCount64();
		auto & window = m_refits[formId];
		if (now - window.first > 10000)
			window = { now, 0 };
		if (++window.second <= 8)
			return true;

		// The first one held back still happens, once the window is over
		if (window.second == 9)
			Refit(formId, UInt32(10000 - (now - window.first)));

		++m_heldBack;
		return false;
	}

	static void Refit(UInt32 formId, UInt32 delay = 0)
	{
		auto refit = [formId]()
		{
			TESForm * form = LookupFormByID(formId);
			if (TESObjectREFR * refr = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr)
				g_bodyMorphInterface.ApplyBodyMorphs(refr, false);
		};

		if (delay)
			MainThreadTasks::AddDelayed(delay, refit);
		else
			MainThreadTasks::AddCoalesced(MainThreadTasks::Kind::UpdateMorphs, formId, nullptr, refit);
	}

	// Under the lock
	void Store(uint64_t key, std::shared_ptr<const MorphCollision::FitMorphs> overlay)
	{
		auto [it, inserted] = m_overlays.try_emplace(key);
		if (!inserted)
			m_bytes -= it->second.overlay->ByteSize();
		it->second = { std::move(overlay), ++m_clock };
		m_bytes += it->second.overlay->ByteSize();

		while (m_bytes > g_morphFitMemoryLimit && m_overlays.size() > 1)
		{
			auto oldest = std::min_element(m_overlays.begin(), m_overlays.end(), [](const auto & a, const auto & b) { return a.second.used < b.second.used; });
			m_bytes -= oldest->second.overlay->ByteSize();
			m_overlays.erase(oldest);
		}
	}

	MorphCollision::FitSettings		m_settings;
	std::filesystem::path			m_folder;
	MorphCollision::BindingCache	m_bindings;

	std::mutex													m_lock;
	std::condition_variable										m_ready;
	std::deque<Job>												m_queue;
	std::unordered_map<uint64_t, Entry>							m_overlays;
	size_t														m_bytes = 0;
	uint64_t													m_clock = 0;
	std::unordered_set<uint64_t>								m_onDisk;
	std::unordered_set<uint64_t>								m_jobs;		// Queued or being made
	std::unordered_map<uint64_t, std::vector<Waiter>>			m_waiters;
	std::unordered_map<uint64_t, uint64_t>						m_latest;	// What each shape waits on
	std::unordered_map<UInt32, UInt32>							m_pending;	// How many overlays each actor waits on
	std::unordered_set<UInt32>									m_refit;	// Actors with overlays grown since their last pass
	std::unordered_map<UInt32, std::pair<ULONGLONG, UInt32>>	m_refits;	// When each actor's refit window began, and how many
	UInt32														m_heldBack = 0;
};

// One shape of an actor, collected and published on the main thread. Compute can run anywhere because it only
// writes to its own block, which keeps everything the game reads whole until Publish swaps it in.
struct MorphJob
{
	NiPointer<NiAVObject>			rootNode;
	NiPointer<BSGeometry>			geometry;
	NiPointer<NiSkinInstance>		skinInstance;
	NiPointer<NiSkinPartition>		partition;
	NiPointer<NiBinaryExtraData>	bodyData;
	const BodyMorphMap *			morphs = nullptr;

	UInt8 *				block = nullptr;
	UInt32				blockSize = 0;
	NiSkinPartition *	copy = nullptr;	// DeepCopy started refcount at 1
	bool				computed = false;
	bool				reference = false;	// Skin only read to fit armor to, it already carries the morphs
	UInt32				conformed = 0;
	UInt32				pushed = 0;
	bool				overlayRequested = false;

	// A clone of a model shares its partition with the model and all the other clones. Once copied it is held
	// by the skin instance alone, and can be written over from then on rather than copied again.
	bool IsPartitionShared() const
	{
		if (partition->m_uiRefCount > 2) // The skin instance and this job
			return true;

		for (UInt32 p = 0; p < partition->m_uiPartitions; ++p)
		{
			if (partition->m_pkPartitions[p].shapeData->m_RefCount > partition->m_uiPartitions)
				return true;
		}

		return false;
	}

	// Only for what the kernel can read, anything else goes through NormalApplicator
	const MorphKernel::Topology * FindTopology() const
	{
		BSTriShape * triShape = ni_cast(geometry, BSTriShape);
		if (!triShape || ni_cast(geometry, BSDynamicTriShape) || morphs->HasUV()) // UV morphs would outdate the cached UV terms
			return nullptr;

		const UInt32 required = VF_VERTEX | VF_UV | VF_NORMAL | VF_TANGENT;
		const UInt32 flags = NiSkinPartition::GetVertexFlags(geometry->vertexDesc);
		const UInt32 vertexSize = NiSkinPartition::GetVertexSize(geometry->vertexDesc);
		const UInt32 vertexCount = partition->vertexCount;
		if ((flags & required) != required || (flags & VF_UV_2) || triShape->numVertices != vertexCount || vertexSize * vertexCount != blockSize)
			return nullptr;

		std::vector<MorphKernel::TriangleRun> triangles;
		for (UInt32 p = 0; p < partition->m_uiPartitions; ++p)
		{
			auto & pPartition = partition->m_pkPartitions[p];
			triangles.push_back({ pPartition.m_pusTriList, pPartition.m_usTriangles });
		}

		std::vector<UInt16> lockedVertices;
		if (NiIntegersExtraData * extraData = static_cast<NiIntegersExtraData*>(NifUtils::GetExtraData(geometry, "LOCKEDNORM")))
		{
			lockedVertices.reserve(extraData->m_size);
			for (UInt32 i = 0; i < extraData->m_size; ++i)
				lockedVertices.push_back(static_cast<UInt16>(extraData->m_data[i]));
		}

		return morphs->GetTopology(block, vertexCount, { vertexSize, 16, 20, 24 }, triangles, lockedVertices);
	}

	void Compute(const MorphFactors & factors, uint64_t settingsKey, const std::vector<CollisionTarget> & targets, UInt32 formId)
	{
		struct ActiveMorph
		{
			TriShapeVertexData * vertices;
			TriShapeVertexData * uvs;
			float factor;
		};

		std::vector<ActiveMorph> active;
		for (auto & factor : factors)
		{
			auto it = morphs->find(*factor.first);
			if (it != morphs->end())
				active.push_back({ it->second.first.get(), morphs->HasUV() ? it->second.second.get() : nullptr, factor.second });
		}

		// SHAPEDATA holds the shape as it was before any morph, the vertex block may already be morphed even on attach
		const bool existingMorphs = bodyData && bodyData->m_size == blockSize;
		// Without body morphs the skin is at rest, and so is anything fitted to it
		const bool fits = !targets.empty() && !factors.empty() && !IsSkin(geometry) && skinInstance->m_spSkinData && HasPartitionPositions(geometry);
		if (active.empty() && !existingMorphs && !fits)
			return;

		auto & firstPartition = partition->m_pkPartitions[0];
		memcpy(block, existingMorphs ? reinterpret_cast<UInt8*>(bodyData->m_data) : firstPartition.shapeData->m_RawVertexData, blockSize);

		const MorphKernel::Topology * topology = g_enableBodyNormalRecalculate ? FindTopology() : nullptr;

		// The overlay of this shape for the skin it sits on, kept under the meshes and TRIs. Sliders it hasn't looked at yet
		// are looked at on the overlay threads, until then the shape goes without them.
		const UInt32 stride = NiSkinPartition::GetVertexSize(firstPartition.vertexDesc);
		const UInt32 vertexCount = partition->vertexCount;
		std::shared_ptr<const MorphCollision::FitMorphs> overlay;
		std::vector<const CollisionTarget*> matched;
		if (fits)
		{
			for (auto & target : targets)
			{
				if (target.skeleton == skinInstance->m_pkRootParent && SameTransform(*target.rootParentToSkin, skinInstance->m_spSkinData->m_kRootParentToSkin))
					matched.push_back(&target);
			}

			// Skin comes in a different order when armor attaches than when the whole actor updates
			std::sort(matched.begin(), matched.end(), [](const CollisionTarget * a, const CollisionTarget * b) { return a->key < b->key; });
			matched.erase(std::unique(matched.begin(), matched.end(), [](const CollisionTarget * a, const CollisionTarget * b) { return a->key == b->key; }), matched.end());
		}

		if (!matched.empty())
		{
			const uint64_t armorPrint = MorphCollision::Fingerprint({ block, vertexCount, stride });
			uint64_t key = MorphCollision::Hash(&settingsKey, sizeof(settingsKey), armorPrint);
			const uint64_t signature = TRISignature(morphs);
			key = MorphCollision::Hash(&signature, sizeof(signature), key);
			for (UInt32 p = 0; p < partition->m_uiPartitions; ++p)
				key = MorphCollision::Hash(&partition->m_pkPartitions[p].m_usTriangles, sizeof(UInt16), key);
			for (auto target : matched)
				key = MorphCollision::Hash(&target->key, sizeof(target->key), key);

			overlay = OverlayService::Get().Find(key, vertexCount);
			if (overlay && overlay->skins.size() != matched.size())
				overlay = nullptr;

			// Both in name order
			static const std::vector<std::string> none;
			const auto & lookedAt = overlay ? overlay->looked : none;
			std::vector<std::string> wanted;
			auto looked = lookedAt.begin();
			for (const auto & factor : factors)
			{
				while (looked != lookedAt.end() && MorphCollision::CompareNames(looked->c_str(), factor.first->c_str()) < 0)
					++looked;
				if (looked == lookedAt.end() || MorphCollision::CompareNames(looked->c_str(), factor.first->c_str()) != 0)
					wanted.push_back(factor.first->c_str());
			}

			// The shape is told apart by its actor and its mesh, the game reuses the objects
			const uint64_t shape = MorphCollision::Hash(&formId, sizeof(formId), armorPrint);
			if (!wanted.empty() && OverlayService::Get().Claim(key, formId, shape))
			{
				MorphCollision::FitInput request;
				request.armorRest = MorphCollision::Positions({ block, vertexCount, stride });
				for (UInt32 p = 0; p < partition->m_uiPartitions; ++p)
				{
					auto & run = partition->m_pkPartitions[p];
					request.armorTriangles.insert(request.armorTriangles.end(), run.m_pusTriList, run.m_pusTriList + run.m_usTriangles * 3);
				}
				request.armorTRI = FitTRIOf(morphs, vertexCount);
				for (auto target : matched)
					request.skins.push_back(target->Skin());
				request.wanted = std::move(wanted);
				OverlayService::Get().Submit(key, std::move(request));
				overlayRequested = true;
			}
		}

		TriShapeVertexData::Layout layout;
		layout.vertexDesc = firstPartition.shapeData->m_VertexDesc;
		layout.vertexData = block;

		for (auto & morph : active)
		{
			if (morph.vertices)
				morph.vertices->ApplyMorph(partition->vertexCount, &layout, morph.factor);
			if (morph.uvs)
				morph.uvs->ApplyMorph(partition->vertexCount, &layout, morph.factor);
		}

		// The overlay's morphs on top of the shape's own, at the same values, both in name order
		if (overlay)
		{
			auto slider = overlay->sliders.begin();
			for (const auto & factor : factors)
			{
				while (slider != overlay->sliders.end() && MorphCollision::CompareNames(slider->name.c_str(), factor.first->c_str()) < 0)
					++slider;
				if (slider != overlay->sliders.end() && MorphCollision::CompareNames(slider->name.c_str(), factor.first->c_str()) == 0)
				{
					slider->Apply(block, stride, factor.second);
					++conformed;
				}
			}

			if (g_morphCollision)
			{
				for (size_t k = 0; k < matched.size(); ++k)
					pushed += overlay->skins[k].Resolve(matched[k]->morphed, block, stride, g_morphCollisionClearance);
			}
		}

		// Nothing morphed or fitted, the shape stays as it is
		if (active.empty() && !existingMorphs && !conformed && !pushed)
			return;

		if (topology)
		{
			static thread_local MorphKernel::Scratch scratch;
			topology->Recalculate(block, scratch);
		}
		else if (g_enableBodyNormalRecalculate)
		{
			NormalApplicator applicator(geometry, partition, block);
			applicator.Apply();
		}

		if (IsPartitionShared())
		{
			NifUtils::DeepCopy(partition, (NiObject**)&copy);
			if (!copy)
				return;
		}

		computed = true;
	}

	void Publish(TESObjectREFR * refr)
	{
		if (!computed)
			return;

		// No existing morphs, keep the current vertex block
		if (!bodyData)
		{
			auto & firstPartition = partition->m_pkPartitions[0];
			bodyData = NiBinaryExtraData::Create("SHAPEDATA", reinterpret_cast<char*>(firstPartition.shapeData->m_RawVertexData), blockSize);
			geometry->AddExtraData(bodyData);
		}

		g_bodyMorphInterface.ForEachMorphShapeCallback([&](IBodyMorphInterface::MorphShapeCallback cb)
		{
			cb(refr, rootNode, geometry, partition, bodyData);
		});

		NIOVTaskUpdateSkinPartition update(skinInstance, copy ? copy : partition.get(), g_bodyMorphGPUCopy, g_bodyMorphRebind, block);
		update.Run();

		if (copy)
			copy->DecRef(); // Ownership passed to the skin instance
	}
};

static void AddJob(std::vector<MorphJob> & jobs, NiAVObject * rootNode, BSGeometry * geometry, const BodyMorphMap & morphs, bool isAttaching)
{
	// Armor covering several biped slots comes up once per slot
	for (auto & job : jobs)
	{
		if (job.geometry.get() == geometry)
			return;
	}

	NiPointer<NiSkinInstance> skinInstance = niptr_cast<NiSkinInstance>(geometry->m_spSkinInstance);
	if (!skinInstance)
		return;

	NiPointer<NiSkinPartition> skinPartition = niptr_cast<NiSkinPartition>(skinInstance->m_spSkinPartition);
	if (!skinPartition || !skinPartition->m_uiPartitions)
		return;

	MorphJob job;
	job.rootNode = rootNode;
	job.geometry = geometry;
	job.skinInstance = skinInstance;
	job.partition = skinPartition;
	job.bodyData = (NiBinaryExtraData*)NifUtils::GetExtraData(geometry, "SHAPEDATA");
	job.morphs = &morphs;
	if (isAttaching && job.bodyData)
		_MESSAGE("%s - %s was attached again with its morphs already applied", __FUNCTION__, geometry->m_name ? geometry->m_name : "");
	job.blockSize = skinPartition->vertexCount * skinPartition->GetVertexSize(skinPartition->m_pkPartitions[0].vertexDesc);
	jobs.push_back(std::move(job));
}

void MorphFileCache::CollectJobs(NiAVObject * rootNode, bool isAttaching, std::vector<MorphJob> & jobs)
{
	auto findMorphs = [&](BSGeometry * geometry)
	{
		return vertexMap.find(SKEEFixedString(geometry->m_name ? geometry->m_name : ""));
	};

	if (BSGeometry * geometry = rootNode->GetAsBSGeometry())
	{
		// The root is the one shape there is, whatever its name
		auto it = findMorphs(geometry);
		AddJob(jobs, rootNode, geometry, it != vertexMap.end() ? it->second : vertexMap.begin()->second, isAttaching);
		return;
	}

	VisitObjects(rootNode, [&](NiAVObject * object)
	{
		if (BSGeometry * geometry = object->GetAsBSGeometry())
		{
			auto it = findMorphs(geometry);
			if (it != vertexMap.end())
				AddJob(jobs, rootNode, geometry, it->second, isAttaching);
		}
		return false;
	});
}

void MorphFileCache::ForEachShape(std::function<void(const SKEEFixedString&, const BodyMorphMap&)> functor) const
{
	for (auto it : vertexMap)
	{
		functor(it.first, it.second);
	}
}

// Morphing rebuilds every shape of an actor, and mods tend to request it many times in a row.
// Pending requests for the same target collapse into one pass on the main thread.
void MorphCache::ApplyMorphs(TESObjectREFR * refr, NiAVObject * rootNode, bool isAttaching, bool deferUpdate)
{
	if (!refr || !rootNode)
		return;

	if (!deferUpdate && MainThreadTasks::IsMainThread())
	{
		ApplyMorphsNow(refr, rootNode, isAttaching);
		return;
	}

	UInt32 formId = refr->formID;
	NiPointer<NiAVObject> object = rootNode;
	NiNode * parent = rootNode->m_parent;
	auto kind = isAttaching ? MainThreadTasks::Kind::ApplyMorphsAttaching : MainThreadTasks::Kind::ApplyMorphs;
	MainThreadTasks::AddCoalesced(kind, formId, rootNode, [this, formId, refr, object, parent, isAttaching]()
	{
		if (LookupFormByID(formId) == refr && object->m_parent == parent)
			ApplyMorphsNow(refr, object, isAttaching);
	});
}

void MorphCache::CollectJobs(NiAVObject * rootNode, bool isAttaching, std::vector<MorphJob> & jobs)
{
	MorphFileCache * fileCache = nullptr;

	// Find the BODYTRI and cache it
	VisitObjects(rootNode, [&](NiAVObject* object) {
		NiStringExtraData * stringData = ni_cast(NifUtils::GetExtraData(object, "BODYTRI"), NiStringExtraData);
		if (stringData) {
			SKEEFixedString filePath = CreateTRIPath(stringData->m_pString);
			CacheFile(filePath.c_str());
			auto it = m_data.find(filePath);
			if (it != m_data.end()) {
				fileCache = &it->second;
				return true;
			}
		}

		return false;
	});

	if (fileCache && !fileCache->vertexMap.empty())
		fileCache->CollectJobs(rootNode, isAttaching, jobs);

	// Shapes no TRI covers get morph maps without morphs, so every slider counts as missing and conform fits them
	if (g_morphConform && g_morphConformWithoutTRI)
	{
		VisitGeometry(rootNode, [&](BSGeometry * geometry)
		{
			if (!IsSkin(geometry) && ConformsWithoutTRI(geometry))
				AddJob(jobs, rootNode, geometry, m_conformOnly[SKEEFixedString(geometry->m_name ? geometry->m_name : "")], isAttaching);
			return false;
		});
	}
}

static std::vector<CollisionTarget> CollectCollisionTargets(TESObjectREFR * refr, const std::vector<MorphJob> & jobs)
{
	std::vector<CollisionTarget> targets;
	absl::flat_hash_set<BSGeometry*> seen;

	for (auto & job : jobs)
	{
		if (IsSkin(job.geometry) && seen.insert(job.geometry).second)
			AddCollisionTarget(targets, job.geometry, job.computed ? job.block : nullptr, job.morphs);
	}

	// Skin left out of this run has no TRI, so it never moves
	VisitBipedNodes(refr, [&](bool isFirstPerson, UInt32 bipedIndex, NiNode * rootNode, TESForm * armo, TESForm * arma, NiAVObject * object)
	{
		VisitGeometry(object, [&](BSGeometry * geometry)
		{
			if (IsSkin(geometry) && seen.insert(geometry).second)
				AddCollisionTarget(targets, geometry, nullptr, nullptr);
			return false;
		});
	});

	return targets;
}

void MorphCache::RunJobs(TESObjectREFR * refr, std::vector<MorphJob> & jobs, std::chrono::steady_clock::time_point start)
{
	if (jobs.empty())
		return;

	const auto collected = std::chrono::steady_clock::now();

	// Morph callbacks can land back here, a nested run then simply starts with no blocks of its own
	std::vector<UInt8> blocks = std::move(m_vertexBlocks);

	size_t totalSize = 0;
	for (auto & job : jobs)
		totalSize += job.blockSize;
	blocks.resize(totalSize);

	UInt8 * block = blocks.data();
	for (auto & job : jobs)
	{
		job.block = block;
		block += job.blockSize;
	}

	// In name order, as overlays keep their sliders, so the two can be walked together
	MorphFactors factors = g_bodyMorphInterface.GetMorphFactors(refr);
	std::sort(factors.begin(), factors.end(), [](const auto & a, const auto & b) { return MorphCollision::CompareNames(a.first->c_str(), b.first->c_str()) < 0; });

	// Overlays are kept under the settings they were made with, along with the meshes and TRIs
	const float settings[] = { float(g_morphConform), float(g_morphCollision), g_morphCollisionDistance, g_morphConformDistance, g_morphConformMismatch };
	const uint64_t settingsKey = MorphCollision::Hash(settings, sizeof(settings));

	auto compute = [&](const std::vector<MorphJob*> & batch, const std::vector<CollisionTarget> & targets)
	{
		if (g_parallelMorphing)
			concurrency::parallel_for(size_t(0), batch.size(), [&](size_t i) { batch[i]->Compute(factors, settingsKey, targets, refr->formID); });
		else
			for (auto job : batch)
				job->Compute(factors, settingsKey, targets, refr->formID);
	};

	// Skin is morphed first, armor is pushed out of it as this update leaves it
	std::vector<MorphJob*> skin, others;
	for (auto & job : jobs)
	{
		if (!job.reference)
			(FitsArmor() && IsSkin(job.geometry) ? skin : others).push_back(&job);
	}

	compute(skin, {});

	std::vector<CollisionTarget> targets;
	if (FitsArmor())
		targets = CollectCollisionTargets(refr, jobs);

	compute(others, targets);

	const auto computed = std::chrono::steady_clock::now();

	UInt32 copies = 0, conformed = 0, pushed = 0, requested = 0;
	for (auto & job : jobs)
	{
		copies += job.copy ? 1 : 0;
		requested += job.overlayRequested ? 1 : 0;
		conformed += job.conformed;
		pushed += job.pushed;
		job.Publish(refr);
	}

	m_vertexBlocks = std::move(blocks);

	if (g_logMorphTiming)
	{
		const auto milliseconds = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };
		_MESSAGE("%s - %08X: %u shapes, %u copied, %u factors, %u skin targets, %u overlay sliders applied, %u vertices pushed, %u overlays grown, %u refits held back, overlays %zu KB, collect %.3f ms, compute %.3f ms, publish %.3f ms", __FUNCTION__, refr->formID,
			(UInt32)jobs.size(), copies, (UInt32)factors.size(), (UInt32)targets.size(), conformed, pushed, requested, FitsArmor() ? OverlayService::Get().TakeHeldBack() : 0, FitsArmor() ? OverlayService::Get().ByteSize() / 1024 : 0,
			milliseconds(start, collected), milliseconds(collected, computed), milliseconds(computed, std::chrono::steady_clock::now()));
	}
}

void MorphCache::ApplyMorphsNow(TESObjectREFR * refr, NiAVObject * rootNode, bool isAttaching)
{
	const auto start = std::chrono::steady_clock::now();

	SimpleLocker locker(&m_lock);

	std::vector<MorphJob> jobs;
	CollectJobs(rootNode, isAttaching, jobs);
	const bool attachedSkin = std::any_of(jobs.begin(), jobs.end(), [](const MorphJob & job) { return IsSkin(job.geometry); });

	// Armor is fitted to the skin elsewhere on the actor, which is only read: it was morphed when it attached or the
	// morphs last changed, and this run needs its TRI for the sliders armor lacks
	if (FitsArmor())
	{
		const size_t attached = jobs.size();
		VisitBipedNodes(refr, [&](bool isFirstPerson, UInt32 bipedIndex, NiNode * root, TESForm * armo, TESForm * arma, NiAVObject * object)
		{
			if (object != rootNode && ContainsSkin(object))
				CollectJobs(object, false, jobs);
		});

		jobs.erase(std::remove_if(jobs.begin() + attached, jobs.end(), [](const MorphJob & job) { return !IsSkin(job.geometry); }), jobs.end());
		for (size_t i = attached; i < jobs.size(); ++i)
		{
			jobs[i].reference = true;
			jobs[i].blockSize = 0;
		}
	}

	RunJobs(refr, jobs, start);

	Shrink();

	// Armor already on the actor was fitted to the skin as it was before
	if (FitsArmor() && attachedSkin)
		UpdateMorphs(refr, true);
}

void MorphCache::UpdateMorphs(TESObjectREFR * refr, bool deferUpdate)
{
	if(!refr)
		return;

	if (!deferUpdate && MainThreadTasks::IsMainThread())
	{
		UpdateMorphsNow(refr);
		return;
	}

	UInt32 formId = refr->formID;
	MainThreadTasks::AddCoalesced(MainThreadTasks::Kind::UpdateMorphs, formId, nullptr, [this, formId]()
	{
		TESForm * form = LookupFormByID(formId);
		if (TESObjectREFR * current = form ? DYNAMIC_CAST(form, TESForm, TESObjectREFR) : nullptr)
			UpdateMorphsNow(current);
	});
}

void MorphCache::UpdateMorphsNow(TESObjectREFR * refr)
{
	Actor * actor = DYNAMIC_CAST(refr, TESObjectREFR, Actor);
	if (!actor)
		return;

	const auto start = std::chrono::steady_clock::now();

	SimpleLocker locker(&m_lock);

	// Every shape of the actor goes into one run, so the workers get to spread out over all of them
	std::vector<MorphJob> jobs;
	VisitBipedNodes(refr, [&](bool isFirstPerson, UInt32 bipedIndex, NiNode* rootNode, TESForm* armo, TESForm* arma, NiAVObject* object)
	{
		CollectJobs(object, false, jobs);
	});

	BSFixedString attachmentName(AttachmentInterface::ATTACHMENT_HOLDER);
	VisitSkeletalRoots(refr, [&](NiNode* rootNode, bool isFirstPerson)
	{
		NiAVObject* attachmentNode = rootNode->GetObjectByName(&attachmentName.data);
		if (attachmentNode)
		{
			CollectJobs(attachmentNode, false, jobs);
		}
	});

	RunJobs(refr, jobs, start);

	Shrink();
}

void MorphCache::ForEachMorphFile(std::function<void(const SKEEFixedString&, const MorphFileCache&)> functor) const
{
	SimpleLocker locker(&m_lock);
	for (auto& it : m_data)
	{
		functor(it.first, it.second);
	}
}

SKEEFixedString MorphCache::CreateTRIPath(const char * relativePath)
{
	if(relativePath == "")
		return SKEEFixedString("");

	std::string targetPath = "meshes\\";
	targetPath += std::string(relativePath);
	std::transform(targetPath.begin(), targetPath.end(), targetPath.begin(), ::tolower);
	return SKEEFixedString(targetPath.c_str());
}

void MorphCache::Shrink()
{
	SimpleLocker locker(&m_lock);
	while (totalMemory > memoryLimit && m_data.size() > 0)
	{
		auto it = std::min_element(m_data.begin(), m_data.end(), [](const auto & a, const auto & b)
		{
			return (a.second.accessed < b.second.accessed);
		});

		size_t size = it->second.vertexMap.memoryUsage;
		m_data.erase(it);
		totalMemory -= size;
	}

	if (m_data.size() == 0) // Just in case we erased but messed up
		totalMemory = sizeof(MorphCache);

	// ponytail: drops every topology of armor without a TRI past a count, they rebuild in a few milliseconds each
	size_t topologies = 0;
	for (auto & [name, morphs] : m_conformOnly)
		topologies += morphs.m_topologies->meshes.size();
	if (topologies > 64)
		m_conformOnly.clear();
}

size_t MorphCache::Clear()
{
	SimpleLocker locker(&m_lock);
	size_t usage = totalMemory;
	totalMemory = 0;
	m_data.clear();
	m_conformOnly.clear();
	OverlayService::Get().Clear();
	return usage;
}

bool MorphCache::CacheFile(const char * relativePath)
{
	SKEEFixedString filePath(relativePath);
	if(relativePath == "")
		return false;

	auto it = m_data.find(filePath);
	if (it != m_data.end()) {
		it->second.accessed = std::time(nullptr);
		it->second.accessed = std::time(nullptr);
		return false;
	}

#ifdef _DEBUG
	_DMESSAGE("%s - Parsing: %s", __FUNCTION__, filePath.c_str());
#endif

	BSResourceNiBinaryStream binaryStream(filePath.c_str());
	if(binaryStream.IsValid())
	{
		TriShapeMap trishapeMap;

		UInt32 fileFormat = 0;
		trishapeMap.memoryUsage += binaryStream.Read((char *)&fileFormat, sizeof(UInt32));

		bool packed = false;
		if (fileFormat != 'TRI\0' && fileFormat != 'TRIP')
			return false;

		if (fileFormat == 'TRIP')
			packed = true;

		UInt32 trishapeCount = 0;
		if (!packed)
			trishapeMap.memoryUsage += binaryStream.Read((char *)&trishapeCount, sizeof(UInt32));
		else
			trishapeMap.memoryUsage += binaryStream.Read((char *)&trishapeCount, sizeof(UInt16));

		char trishapeNameRaw[MAX_PATH];
		for (UInt32 i = 0; i < trishapeCount; i++)
		{
			memset(trishapeNameRaw, 0, MAX_PATH);

			UInt8 size = 0;
			trishapeMap.memoryUsage += binaryStream.Read((char *)&size, sizeof(UInt8));
			trishapeMap.memoryUsage += binaryStream.Read(trishapeNameRaw, size);
			BSFixedString trishapeName(trishapeNameRaw);

#ifdef _DEBUG
			_DMESSAGE("%s - Reading TriShape %s", __FUNCTION__, trishapeName.data);
#endif

			if (!packed) {
				UInt32 trishapeBlockSize = 0;
				trishapeMap.memoryUsage += binaryStream.Read((char *)&trishapeBlockSize, sizeof(UInt32));
			}

			char morphNameRaw[MAX_PATH];

			BodyMorphMap morphMap;

			UInt32 morphCount = 0;
			if (!packed)
				trishapeMap.memoryUsage += binaryStream.Read((char *)&morphCount, sizeof(UInt32));
			else
				trishapeMap.memoryUsage += binaryStream.Read((char *)&morphCount, sizeof(UInt16));

			for (UInt32 j = 0; j < morphCount; j++)
			{
				memset(morphNameRaw, 0, MAX_PATH);

				UInt8 tsize = 0;
				trishapeMap.memoryUsage += binaryStream.Read((char *)&tsize, sizeof(UInt8));
				trishapeMap.memoryUsage += binaryStream.Read(morphNameRaw, tsize);
				BSFixedString morphName(morphNameRaw);

#ifdef _DEBUG
				_DMESSAGE("%s - Reading Morph %s at (%08X)", __FUNCTION__, morphName.data, binaryStream.GetOffset());
#endif
				if (tsize == 0) {
					_WARNING("%s - %s - Read empty name morph at (%08X)", __FUNCTION__, filePath.c_str(), binaryStream.GetOffset());
				}

				if (!packed) {
					UInt32 morphBlockSize = 0;
					trishapeMap.memoryUsage += binaryStream.Read((char *)&morphBlockSize, sizeof(UInt32));
				}

				UInt32 vertexNum = 0;
				float multiplier = 0.0f;
				if(!packed) {
					trishapeMap.memoryUsage += binaryStream.Read((char *)&vertexNum, sizeof(UInt32));
				}
				else {
					trishapeMap.memoryUsage += binaryStream.Read((char *)&multiplier, sizeof(float));
					trishapeMap.memoryUsage += binaryStream.Read((char *)&vertexNum, sizeof(UInt16));
				}

				if (vertexNum == 0) {
					_WARNING("%s - %s - Read morph %s on %s with no vertices at (%08X)", __FUNCTION__, filePath.c_str(), morphName.data, trishapeName.data, binaryStream.GetOffset());
				}
				if (multiplier == 0.0f) {
					_WARNING("%s - %s - Read morph %s on %s with zero multiplier at (%08X)", __FUNCTION__, filePath.c_str(), morphName.data, trishapeName.data, binaryStream.GetOffset());
				}

#ifdef _DEBUG
				_DMESSAGE("%s - Total Vertices read: %d at (%08X)", __FUNCTION__, vertexNum, binaryStream.GetOffset());
#endif
				if (vertexNum > (std::numeric_limits<UInt16>::max)())
				{
					_ERROR("%s - %s - Too many vertices for %s on %s read: %d at (%08X)", __FUNCTION__, filePath.c_str(), morphName.data, vertexNum, trishapeName.data, binaryStream.GetOffset());
					return false;
				}

				TriShapeVertexDataPtr vertexData;
				TriShapeFullVertexDataPtr fullVertexData;
				TriShapePackedVertexDataPtr packedVertexData;
				if (!packed)
				{
					fullVertexData = std::make_shared<TriShapeFullVertexData>();
					for (UInt32 k = 0; k < vertexNum; k++)
					{
						TriShapeVertexDelta vertexDelta;
						float x, y, z;
						trishapeMap.memoryUsage += binaryStream.Read((char *)&vertexDelta.index, sizeof(UInt32));
						trishapeMap.memoryUsage += binaryStream.Read((char *)&x, sizeof(float));
						trishapeMap.memoryUsage += binaryStream.Read((char *)&y, sizeof(float));
						trishapeMap.memoryUsage += binaryStream.Read((char *)&z, sizeof(float));

						vertexDelta.delta = DirectX::XMVectorScale(DirectX::XMVectorSet(x, y, z, 0), multiplier);

						if (vertexDelta.index > fullVertexData->m_maxIndex)
							fullVertexData->m_maxIndex = vertexDelta.index;

						fullVertexData->m_vertexDeltas.push_back(vertexDelta);
					}

					vertexData = fullVertexData;
				}
				else
				{
					packedVertexData = std::make_shared<TriShapePackedVertexData>();
					packedVertexData->m_multiplier = multiplier;
					packedVertexData->m_vertexDeltas.resize(vertexNum);

					trishapeMap.memoryUsage += binaryStream.Read((char *)packedVertexData->m_vertexDeltas.data(), vertexNum * sizeof(TriShapePackedVertexDelta));

					for (const auto & delta : packedVertexData->m_vertexDeltas)
					{
						if (delta.index > packedVertexData->m_maxIndex)
							packedVertexData->m_maxIndex = delta.index;
					}

					vertexData = packedVertexData;
				}

				morphMap.emplace(morphName, std::make_pair(vertexData, nullptr));
			}

			trishapeMap.emplace(trishapeName, morphMap);
		}

		UInt16 UVShapeCount = 0;
		trishapeMap.memoryUsage += binaryStream.Read((char *)&UVShapeCount, sizeof(UInt16));
		for (UInt32 i = 0; i < trishapeCount; i++)
		{
			memset(trishapeNameRaw, 0, MAX_PATH);

			UInt8 size = 0;
			trishapeMap.memoryUsage += binaryStream.Read((char *)&size, sizeof(UInt8));
			trishapeMap.memoryUsage += binaryStream.Read(trishapeNameRaw, size);
			BSFixedString trishapeName(trishapeNameRaw);

#ifdef _DEBUG
			_DMESSAGE("%s - Reading TriShape UV %s", __FUNCTION__, trishapeName.data);
#endif

			if (!packed) {
				UInt32 trishapeBlockSize = 0;
				trishapeMap.memoryUsage += binaryStream.Read((char *)&trishapeBlockSize, sizeof(UInt32));
			}

			char morphNameRaw[MAX_PATH];

			BodyMorphMap uvMorphMap;

			UInt32 morphCount = 0;
			if (!packed)
				trishapeMap.memoryUsage += binaryStream.Read((char *)&morphCount, sizeof(UInt32));
			else
				trishapeMap.memoryUsage += binaryStream.Read((char *)&morphCount, sizeof(UInt16));

			for (UInt32 j = 0; j < morphCount; j++)
			{
				memset(morphNameRaw, 0, MAX_PATH);

				UInt8 tsize = 0;
				trishapeMap.memoryUsage += binaryStream.Read((char *)&tsize, sizeof(UInt8));
				trishapeMap.memoryUsage += binaryStream.Read(morphNameRaw, tsize);
				BSFixedString morphName(morphNameRaw);

#ifdef _DEBUG
				_DMESSAGE("%s - Reading UV Morph %s at (%08X)", __FUNCTION__, morphName.data, binaryStream.GetOffset());
#endif
				if (tsize == 0) {
					_WARNING("%s - %s - Read empty name morph at (%08X)", __FUNCTION__, filePath.c_str(), binaryStream.GetOffset());
				}

				UInt32 vertexNum = 0;
				float multiplier = 0.0f;

				trishapeMap.memoryUsage += binaryStream.Read((char *)&multiplier, sizeof(float));
				trishapeMap.memoryUsage += binaryStream.Read((char *)&vertexNum, sizeof(UInt16));

				if (vertexNum == 0) {
					_WARNING("%s - %s - Read morph %s on %s with no vertices at (%08X)", __FUNCTION__, filePath.c_str(), morphName.data, trishapeName.data, binaryStream.GetOffset());
				}
				if (multiplier == 0.0f) {
					_WARNING("%s - %s - Read morph %s on %s with zero multiplier at (%08X)", __FUNCTION__, filePath.c_str(), morphName.data, trishapeName.data, binaryStream.GetOffset());
				}

#ifdef _DEBUG
				_DMESSAGE("%s - Total Vertices read: %d at (%08X)", __FUNCTION__, vertexNum, binaryStream.GetOffset());
#endif
				if (vertexNum > (std::numeric_limits<UInt16>::max)())
				{
					_ERROR("%s - %s - Too many vertices for %s on %s read: %d at (%08X)", __FUNCTION__, filePath.c_str(), morphName.data, vertexNum, trishapeName.data, binaryStream.GetOffset());
					return false;
				}

				TriShapePackedUVDataPtr packedUVData;
				
				packedUVData = std::make_shared<TriShapePackedUVData>();
				packedUVData->m_multiplier = multiplier;

				for (UInt32 k = 0; k < vertexNum; k++)
				{
					TriShapePackedUVDelta vertexDelta;
					trishapeMap.memoryUsage += binaryStream.Read((char *)&vertexDelta.index, sizeof(UInt16));
					trishapeMap.memoryUsage += binaryStream.Read((char *)&vertexDelta.u, sizeof(SInt16));
					trishapeMap.memoryUsage += binaryStream.Read((char *)&vertexDelta.v, sizeof(SInt16));

					if (vertexDelta.index > packedUVData->m_maxIndex)
						packedUVData->m_maxIndex = vertexDelta.index;

					packedUVData->m_uvDeltas.push_back(vertexDelta);
				}

				trishapeMap[trishapeName][morphName].second = packedUVData;
				trishapeMap[trishapeName].m_hasUV = true;
			}
		}

		MorphFileCache fileCache;
		fileCache.accessed = std::time(nullptr);
		fileCache.vertexMap = trishapeMap;
#ifdef _DEBUG
		fileCache.path = relativePath;
#endif

		Lock();
		m_data.emplace(relativePath, fileCache);
		totalMemory += trishapeMap.memoryUsage;
		Release();

		_DMESSAGE("%s - Loaded %s (%d bytes)", __FUNCTION__, relativePath, trishapeMap.memoryUsage);
		return true;
	}
	else
	{
		_ERROR("%s - Failed to load %s", __FUNCTION__, relativePath);
	}


	return false;
}

void BodyMorphInterface::AddMorphShapeCallback(IBodyMorphInterface::MorphShapeCallback cb, skee_u64 order)
{

}

void BodyMorphInterface::Impl_SetCacheLimit(size_t limit)
{
	morphCache.memoryLimit = limit;
}

void BodyMorphInterface::Impl_ApplyVertexDiff(TESObjectREFR * refr, NiAVObject * rootNode, bool attach)
{
	if(!refr || !rootNode) {
#ifdef _DEBUG
		_DMESSAGE("%s - Error no reference or node found.", __FUNCTION__);
#endif
		return;
	}

#ifdef _DEBUG
	_DMESSAGE("%s - Applying Vertex Diffs to %08X on %s", __FUNCTION__, refr->formID, rootNode->m_name);
#endif
	if (g_enableBodyMorph)
	{
		morphCache.ApplyMorphs(refr, rootNode, attach, true);
	}
}

void BodyMorphInterface::Impl_ApplyBodyMorphs(TESObjectREFR * refr, bool deferUpdate)
{
#ifdef _DEBUG
	_DMESSAGE("%s - Updating morphs for %08X.", __FUNCTION__, refr->formID);
#endif
	if (g_enableBodyMorph)
	{
		morphCache.UpdateMorphs(refr, deferUpdate);
	}
}

NIOVTaskUpdateModelWeight::NIOVTaskUpdateModelWeight(Actor * actor)
{
	m_formId = actor->formID;
}

void NIOVTaskUpdateModelWeight::Dispose(void)
{
	delete this;
}

void NIOVTaskUpdateModelWeight::Run()
{
	TESForm * form = LookupFormByID(m_formId);
	Actor * actor = DYNAMIC_CAST(form, TESForm, Actor);
	if(actor) {
		g_bodyMorphInterface.ApplyBodyMorphs(actor, false);
	}
}

NIOVTaskUpdateSkinPartition::NIOVTaskUpdateSkinPartition(NiSkinInstance * skinInstance, NiSkinPartition * partition, bool gpuCopy, bool rebindDynamic, const UInt8 * vertexBlock)
{
	m_skinInstance = skinInstance;
	m_partition = partition;
	m_vertexBlock = vertexBlock;
	m_copyGPU = gpuCopy;
	m_rebindDynamic = rebindDynamic;
}

void NIOVTaskUpdateSkinPartition::Dispose(void)
{
	delete this;
}

void NIOVTaskUpdateSkinPartition::Run()
{
	if (m_skinInstance && m_partition)
	{
		EnterCriticalSection(&g_renderManager->lock);
		EnterCriticalSection(&m_skinInstance->lock);
		auto & partition = m_partition->m_pkPartitions[0];
		UInt32 vertexSize = m_partition->GetVertexSize(partition.vertexDesc);
		UInt32 vertexCount = m_partition->vertexCount;

		if (m_vertexBlock)
		{
			for (UInt32 p = 0; p < m_partition->m_uiPartitions; ++p)
			{
				UInt8 * vertices = m_partition->m_pkPartitions[p].shapeData->m_RawVertexData;
				if (p == 0 || vertices != partition.shapeData->m_RawVertexData)
					memcpy(vertices, m_vertexBlock, vertexCount * vertexSize);
			}
		}

		D3D11_BUFFER_DESC desc;
		partition.shapeData->m_VertexBuffer->GetDesc(&desc);

		// Rebind functionality to switch buffer to be dynamic, and then re-use it across all partitions
		if (m_rebindDynamic && desc.Usage != D3D11_USAGE_DYNAMIC)
		{
			D3D11_BUFFER_DESC newDesc = desc;
			newDesc.Usage = D3D11_USAGE_DYNAMIC;
			newDesc.CPUAccessFlags |= D3D11_CPU_ACCESS_WRITE;

			ID3D11Buffer* buffer = nullptr;
			D3D11_SUBRESOURCE_DATA data;
			data.pSysMem = partition.shapeData->m_RawVertexData;
			data.SysMemPitch = vertexCount * vertexSize;
			data.SysMemSlicePitch = 0;
			
			if (SUCCEEDED(g_renderManager->forwarder->CreateBuffer(&newDesc, &data, &buffer)))
			{
				// Borrow the exact same partition and skip the copy entirely
				for (UInt32 p = 0; p < m_partition->m_uiPartitions; ++p)
				{
					auto& pPartition = m_partition->m_pkPartitions[p];
					pPartition.shapeData->m_VertexBuffer->Release();
					pPartition.shapeData->m_VertexBuffer = buffer;
					pPartition.shapeData->m_VertexBuffer->AddRef();
				}

				// The partitions will now take ownership
				partition.shapeData->m_VertexBuffer->Release();

				desc = newDesc;
			}
		}
		
		// Perform the resource copy either on CPU or GPU, and only on partitions that differ
		auto deviceContext = g_renderManager->context;
		switch (desc.Usage)
		{
		case D3D11_USAGE_DEFAULT:
		{
			deviceContext->UpdateSubresource(partition.shapeData->m_VertexBuffer, 0, nullptr, partition.shapeData->m_RawVertexData, vertexCount * vertexSize, 0);

			if (m_copyGPU)
			{
				for (UInt32 p = 1; p < m_partition->m_uiPartitions; ++p)
				{
					auto& pPartition = m_partition->m_pkPartitions[p];
					if (pPartition.shapeData->m_VertexBuffer != partition.shapeData->m_VertexBuffer) {
						deviceContext->CopyResource(pPartition.shapeData->m_VertexBuffer, partition.shapeData->m_VertexBuffer);
					}
				}
			}
			else
			{
				for (UInt32 p = 1; p < m_partition->m_uiPartitions; ++p)
				{
					auto& pPartition = m_partition->m_pkPartitions[p];
					if (pPartition.shapeData->m_VertexBuffer != partition.shapeData->m_VertexBuffer) {
						deviceContext->UpdateSubresource(pPartition.shapeData->m_VertexBuffer, 0, nullptr, pPartition.shapeData->m_RawVertexData, vertexCount * vertexSize, 0);
					}
				}
			}
			break;
		}
		case D3D11_USAGE_DYNAMIC:
		{
			if (desc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) {
				D3D11_MAPPED_SUBRESOURCE mappedResource;
				if (deviceContext->Map(partition.shapeData->m_VertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource) == S_OK) {
					memcpy(mappedResource.pData, partition.shapeData->m_RawVertexData, vertexCount * vertexSize);
					deviceContext->Unmap(partition.shapeData->m_VertexBuffer, 0);
				}

				if (m_copyGPU)
				{
					for (UInt32 p = 1; p < m_partition->m_uiPartitions; ++p)
					{
						auto& pPartition = m_partition->m_pkPartitions[p];
						if (pPartition.shapeData->m_VertexBuffer != partition.shapeData->m_VertexBuffer) {
							deviceContext->CopyResource(pPartition.shapeData->m_VertexBuffer, partition.shapeData->m_VertexBuffer);
						}
					}
				}
				else
				{
					for (UInt32 p = 1; p < m_partition->m_uiPartitions; ++p)
					{
						auto& pPartition = m_partition->m_pkPartitions[p];
						if (pPartition.shapeData->m_VertexBuffer != partition.shapeData->m_VertexBuffer) {
							if (deviceContext->Map(pPartition.shapeData->m_VertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource) == S_OK) {
								memcpy(mappedResource.pData, pPartition.shapeData->m_RawVertexData, vertexCount * vertexSize);
								deviceContext->Unmap(pPartition.shapeData->m_VertexBuffer, 0);
							}
						}
					}
				}
			}
			break;
		default:
			_ERROR("%s - Failure to copy morph data into resource", __FUNCTION__);
			break;
		}
		}

		m_skinInstance->m_spSkinPartition = m_partition;
		LeaveCriticalSection(&m_skinInstance->lock);
		LeaveCriticalSection(&g_renderManager->lock);
	}
}

void BodyMorphInterface::Impl_VisitMorphs(TESObjectREFR * actor, std::function<void(SKEEFixedString name, absl::flat_hash_map<StringTableItem, float> * map)> functor)
{
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		for (auto & morph : it->second)
		{
			functor(*morph.first, &morph.second);
		}
	}
}

void BodyMorphInterface::Impl_VisitKeys(TESObjectREFR * actor, SKEEFixedString name, std::function<void(SKEEFixedString, float)> functor)
{
	auto it = actorMorphs.m_data.find(actor->formID);
	if (it != actorMorphs.m_data.end())
	{
		auto mit = it->second.find(g_stringTable.GetString(name));
		if (mit != it->second.end())
		{
			for (auto & morph : mit->second)
			{
				functor(*morph.first, morph.second);
			}
		}
	}
}


void BodyMorphInterface::Impl_UpdateModelWeight(TESObjectREFR * refr, bool immediate)
{
	Actor * actor = DYNAMIC_CAST(refr, TESObjectREFR, Actor);
	if(actor) {
		NIOVTaskUpdateModelWeight * updateTask = new NIOVTaskUpdateModelWeight(actor);
		if (immediate) {
			updateTask->Run();
			updateTask->Dispose();
		}
		else
		{
			MainThreadTasks::Add(updateTask);
		}
	}
}

bool BodyMorphInterface::Impl_ReadBodyMorphTemplates(SKEEFixedString filePath)
{
	BSResourceNiBinaryStream file(filePath.c_str());
	if (!file.IsValid()) {
		return false;
	}

	BSResourceTextFile<0x7FFF> textFile(&file);

	UInt32 lineCount = 0;
	std::string str = "";
	UInt32 loadedTemplates = 0;

	while (textFile.ReadLine(&str))
	{
		lineCount++;
		str = std::trim(str);
		if (str.length() == 0)
			continue;
		if (str.at(0) == '#')
			continue;

		std::vector<std::string> side = std::explode(str, '=');
		if (side.size() < 2) {
			_ERROR("%s - Error - Template has no left-hand side.\tLine (%d) [%s]", __FUNCTION__, lineCount, filePath.c_str());
			continue;
		}

		std::string lSide = std::trim(side[0]);
		std::string rSide = std::trim(side[1]);

		BSFixedString templateName = lSide.c_str();

		BodyGenTemplatePtr bodyGenSets = std::make_shared<BodyGenTemplate>();

		std::string error = "";
		std::vector<std::string> sets = std::explode(rSide, '/');
		for (UInt32 i = 0; i < sets.size(); i++) {
			sets[i] = std::trim(sets[i]);

			BodyGenMorphs bodyMorphs;

			std::vector<std::string> morphs = std::explode(sets[i], ',');
			for (UInt32 j = 0; j < morphs.size(); j++) {
				morphs[j] = std::trim(morphs[j]);

				std::vector<std::string> selectors = std::explode(morphs[j], '|');

				BodyGenMorphSelector selector;

				for (UInt32 k = 0; k < selectors.size(); k++) {
					selectors[k] = std::trim(selectors[k]);

					std::vector<std::string> pairs = std::explode(selectors[k], '@');
					if (pairs.size() < 2) {
						error = "Must have value pair with @ (";
						error += selectors[k];
						error += ")";
						break;
					}

					std::string morphName = std::trim(pairs[0]);
					if (morphName.length() == 0) {
						error = "Empty morph name";
						break;
					}

					std::string morphValues = std::trim(pairs[1]);
					if (morphValues.length() == 0) {
						error = "Empty values for (";
						error += morphName;
						error += ")";
						break;
					}

					float lowerValue = 0;
					float upperValue = 0;

					std::vector<std::string> range = std::explode(morphValues, ':');
					if (range.size() > 1) {
						std::string lowerRange = std::trim(range[0]);
						if (lowerRange.length() == 0) {
							error = "Empty lower range for (";
							error += morphName;
							error += ")";
							break;
						}

						lowerValue = atof(lowerRange.c_str());

						std::string upperRange = std::trim(range[1]);
						if (upperRange.length() == 0) {
							error = "Empty upper range for (";
							error += morphName;
							error += ")";
							break;
						}

						upperValue = atof(upperRange.c_str());
					}
					else {
						lowerValue = atof(morphValues.c_str());
						upperValue = lowerValue;
					}

					BodyGenMorphData morphData;
					morphData.name = morphName.c_str();
					morphData.lower = lowerValue;
					morphData.upper = upperValue;
					selector.push_back(morphData);
				}

				if (error.length() > 0)
					break;

				bodyMorphs.push_back(selector);
			}

			if (error.length() > 0)
				break;

			bodyGenSets->push_back(bodyMorphs);
		}

		if (error.length() > 0) {
			_ERROR("%s - Error - Could not parse morphs %s.\tLine (%d) [%s]", __FUNCTION__, error.c_str(), lineCount, filePath.c_str());
			continue;
		}

		if (bodyGenSets->size() > 0) {
			bodyGenTemplates[templateName] = bodyGenSets;
			loadedTemplates++;
		}
	}

	_MESSAGE("%s - Info - Loaded %d template(s).\t[%s]", __FUNCTION__, loadedTemplates, filePath.c_str());
	return true;
}

void BodyMorphInterface::GetFilteredNPCList(std::vector<TESNPC*> activeNPCs[], const ModInfo * modInfo, UInt32 gender, TESRace * raceFilter, absl::flat_hash_set<TESFaction*> factionList)
{
	for (UInt32 i = 0; i < (*g_dataHandler)->npcs.count; i++)
	{
		TESNPC * npc = nullptr;
		if ((*g_dataHandler)->npcs.GetNthItem(i, npc))
		{
			bool matchMod = modInfo ? modInfo->IsFormInMod(npc->formID) : true;
			bool matchRace = (raceFilter == nullptr || npc->race.race == raceFilter);			
			bool matchFactions = IsNPCInFactions(npc, factionList);

			if (npc && npc->nextTemplate == nullptr && matchMod && matchRace && matchFactions)
			{
				if (gender == 0xFF)
				{
					activeNPCs[0].push_back(npc);
					activeNPCs[1].push_back(npc);
				}
				else
					activeNPCs[gender].push_back(npc);
			}
		}
	}
}

bool BodyMorphInterface::IsNPCInFactions(TESNPC* npc, absl::flat_hash_set<TESFaction*> factionList)
{
	UInt32 matchingFactions = 0;
	if (factionList.size() > 0)
	{
		for (UInt32 k = 0; k < npc->actorData.factions.count; ++k)
		{
			TESActorBaseData::FactionInfo fi;
			npc->actorData.factions.GetNthItem(k, fi);
			if (factionList.find(fi.faction) != factionList.end())
				matchingFactions++;
		}
	}

	return matchingFactions == factionList.size();
}

bool BodyMorphInterface::Impl_ReadBodyMorphs(SKEEFixedString filePath)
{
	BSResourceNiBinaryStream file(filePath.c_str());
	if (!file.IsValid()) {
		return false;
	}

	BSResourceTextFile<0x7FFF> textFile(&file);

	UInt32 lineCount = 0;
	std::string str = "";
	UInt32 maleTargets = 0;
	UInt32 femaleTargets = 0;
	UInt32 maleOverwrite = 0;
	UInt32 femaleOverwrite = 0;

	while (textFile.ReadLine(&str))
	{
		lineCount++;
		str = std::trim(str);
		if (str.length() == 0)
			continue;
		if (str.at(0) == '#')
			continue;

		std::vector<std::string> side = std::explode(str, '=');
		if (side.size() < 1) {
			_ERROR("%s - Error - Morph has no left-hand side.\tLine (%d) [%s]", __FUNCTION__, lineCount, filePath.c_str());
			continue;
		}

		absl::flat_hash_set<TESFaction*> factionList;

		std::string lSide = std::trim(side[0]);
		std::vector<std::string> lProperties = std::explode(lSide, ',');
		if (lProperties.size() > 1)
		{
			std::vector<std::string> forms = std::explode(std::trim(lProperties[1]), '+');
			for (auto & formIdentifier : forms)
			{
				TESForm * form = GetFormFromIdentifier(std::trim(formIdentifier));
				if (form && form->formType == TESFaction::kTypeID)
				{
					factionList.insert(static_cast<TESFaction*>(form));
				}
			}
		}

		std::string rSide = side.size() > 1 ? std::trim(side[1]) : "";

		std::vector<std::string> form = std::explode(lProperties[0], '|');
		if (form.size() < 2) {
			_ERROR("%s - Error - Morph left side missing mod name or formID.\tLine (%d) [%s]", __FUNCTION__, lineCount, filePath.c_str());
			continue;
		}

		int paramIndex = 0;

		std::vector<TESNPC*> activeNPCs[2];
		std::string modNameText = std::trim(form[paramIndex]);
		paramIndex++;

		// All|Gender[|Race]
		if (_strnicmp(modNameText.c_str(), "all", 3) == 0)
		{
			UInt8 gender = 0xFF;
			if (form.size() > paramIndex)
			{
				std::string genderText = std::trim(form[paramIndex]);
				std::transform(genderText.begin(), genderText.end(), genderText.begin(), ::tolower);
				if (genderText.compare("male") == 0) {
					gender = 0;
					paramIndex++;
				}
				else if (genderText.compare("female") == 0) {
					gender = 1;
					paramIndex++;
				}
			}

			TESRace * foundRace = nullptr;
			if (form.size() > paramIndex)
			{
				std::string raceText = std::trim(form[paramIndex]);
				foundRace = GetRaceByName(raceText);
				if (foundRace == nullptr)
				{
					_ERROR("%s - Error - Invalid race %s specified.\tLine (%d) [%s]", __FUNCTION__, raceText.c_str(), lineCount, filePath.c_str());
					continue;
				}
				paramIndex++;
			}

			GetFilteredNPCList(activeNPCs, nullptr, gender, foundRace, factionList);
		}
		else
		{
			const ModInfo * modInfo = (*g_dataHandler)->LookupModByName(modNameText.c_str());
			if (!modInfo || !modInfo->IsActive()) {
				_WARNING("%s - Warning - Mod '%s' not a loaded mod.\tLine (%d) [%s]", __FUNCTION__, modNameText.c_str(), lineCount, filePath.c_str());
				continue;
			}

			TESForm * foundForm = nullptr;
			std::string formIdText = std::trim(form[paramIndex]);
			paramIndex++;

			UInt8 gender = 0xFF;
			if (form.size() > paramIndex) {
				std::string genderText = std::trim(form[paramIndex]);
				std::transform(genderText.begin(), genderText.end(), genderText.begin(), ::tolower);
				if (genderText.compare("male") == 0) {
					gender = 0;
					paramIndex++;
				}
				else if (genderText.compare("female") == 0) {
					gender = 1;
					paramIndex++;
				}
			}

			// Fallout4.esm|All[|Gender][|Race]
			if (_strnicmp(formIdText.c_str(), "all", 3) == 0)
			{
				TESRace * foundRace = nullptr;
				if (form.size() > paramIndex)
				{
					std::string raceText = std::trim(form[paramIndex]);
					foundRace = GetRaceByName(raceText);
					if (foundRace == nullptr)
					{
						_ERROR("%s - Error - Invalid race '%s' specified.\tLine (%d) [%s]", __FUNCTION__, raceText.c_str(), lineCount, filePath.c_str());
						continue;
					}
					paramIndex++;
				}

				GetFilteredNPCList(activeNPCs, modInfo, gender, foundRace, factionList);
			}
			else // Fallout4.esm|XXXX[|Gender]
			{
				UInt32 formLower = strtoul(formIdText.c_str(), NULL, 16);
				if (formLower == 0) {
					_ERROR("%s - Error - Invalid formID.\tLine (%d) [%s]", __FUNCTION__, lineCount, filePath.c_str());
					continue;
				}

				UInt32 formId = modInfo->GetFormID(formLower);
				foundForm = LookupFormByID(formId);
				if (!foundForm) {
					_ERROR("%s - Error - Invalid form %08X.\tLine (%d) [%s]", __FUNCTION__, formId, lineCount, filePath.c_str());
					continue;
				}
			}

			if (foundForm)
			{
				TESLevCharacter * levCharacter = DYNAMIC_CAST(foundForm, TESForm, TESLevCharacter);
				if (levCharacter) {
					VisitLeveledCharacter(levCharacter, [&](TESNPC * npc)
					{
						if (IsNPCInFactions(npc, factionList))
						{
							if (gender == 0xFF) {
								activeNPCs[0].push_back(npc);
								activeNPCs[1].push_back(npc);
							}
							else
								activeNPCs[gender].push_back(npc);
						}
					});
				}

				TESNPC * npc = DYNAMIC_CAST(foundForm, TESForm, TESNPC);
				if (npc) {
					if (IsNPCInFactions(npc, factionList))
					{
						if (gender == 0xFF) {
							activeNPCs[0].push_back(npc);
							activeNPCs[1].push_back(npc);
						}
						else
							activeNPCs[gender].push_back(npc);
					}
				}

				if (!npc && !levCharacter) {
					_ERROR("%s - Error - Invalid form %08X not an ActorBase or LeveledActor.\tLine (%d) [%s]", __FUNCTION__, foundForm->formID, lineCount, filePath.c_str());
					continue;
				}
			}
		}

		BodyGenDataTemplatesPtr dataTemplates = std::make_shared<BodyGenDataTemplates>();
		std::string error = "";
		std::vector<std::string> sets = std::explode(rSide, ',');
		for (UInt32 i = 0; i < sets.size(); i++) {
			sets[i] = std::trim(sets[i]);
			std::vector<std::string> selectors = std::explode(sets[i], '|');
			BodyTemplateList templateList;
			for (UInt32 k = 0; k < selectors.size(); k++) {
				selectors[k] = std::trim(selectors[k]);
				BSFixedString templateName(selectors[k].c_str());
				auto temp = bodyGenTemplates.find(templateName);
				if (temp != bodyGenTemplates.end())
					templateList.push_back(temp->second);
				else
					_WARNING("%s - Warning - template %s not found.\tLine (%d) [%s]", __FUNCTION__, templateName.c_str(), lineCount, filePath.c_str());
			}

			dataTemplates->push_back(templateList);
		}

		for (auto & npc : activeNPCs[0])
		{
			if (bodyGenData[0].find(npc) == bodyGenData[0].end()) {
#ifdef _DEBUG
				_DMESSAGE("%s - Read male target %s (%08X)", __FUNCTION__, npc->fullName.name.c_str(), npc->formID);
#endif
				maleTargets++;
			}
			else {
				maleOverwrite++;
			}

			bodyGenData[0][npc] = dataTemplates;
		}

		for (auto & npc : activeNPCs[1])
		{
			if (bodyGenData[1].find(npc) == bodyGenData[1].end()) {
#ifdef _DEBUG
				_DMESSAGE("%s - Read female target %s (%08X)", __FUNCTION__, npc->fullName.name.c_str(), npc->formID);
#endif
				femaleTargets++;
			}
			else {
				femaleOverwrite++;
			}

			bodyGenData[1][npc] = dataTemplates;
		}

		if (maleOverwrite)
			_MESSAGE("%s - Info - %d male NPC targets(s) overwritten.\tLine (%d) [%s]", __FUNCTION__, maleOverwrite, lineCount, filePath.c_str());
		if (femaleOverwrite)
			_MESSAGE("%s - Info - %d female NPC targets(s) overwritten.\tLine (%d) [%s]", __FUNCTION__, femaleOverwrite, lineCount, filePath.c_str());

		maleOverwrite = 0;
		femaleOverwrite = 0;
	}

	_MESSAGE("%s - Info - Acquired %d male NPC target(s).\t[%s]", __FUNCTION__, maleTargets, filePath.c_str());
	_MESSAGE("%s - Info - Acquired %d female NPC target(s).\t[%s]", __FUNCTION__, femaleTargets, filePath.c_str());
	return true;
}

UInt32 BodyGenMorphSelector::Evaluate(std::function<void(SKEEFixedString, float)> eval)
{
	if (size() > 0) {
		std::random_device rd;
		std::default_random_engine gen(rd());
		std::uniform_int_distribution<> rndMorph(0, size() - 1);

		auto & bodyMorph = at(rndMorph(gen));
		std::uniform_real_distribution<> rndValue(bodyMorph.lower, bodyMorph.upper);
		float val = rndValue(gen);
		if (val != 0) {
			eval(bodyMorph.name, val);
			return 1;
		}
	}

	return 0;
}

UInt32 BodyGenMorphs::Evaluate(std::function<void(SKEEFixedString, float)> eval)
{
	UInt32 total = 0;
	for (auto value : *this) {
		if (value.size() < 1)
			continue;

		total += value.Evaluate(eval);
	}

	return total;
}

UInt32 BodyGenTemplate::Evaluate(std::function<void(SKEEFixedString, float)> eval)
{
	if (size() > 0) {
		std::random_device rd;
		std::default_random_engine gen(rd());
		std::uniform_int_distribution<> rnd(0, size() - 1);

		auto & morphs = at(rnd(gen));
		return morphs.Evaluate(eval);
	}

	return 0;
}

UInt32 BodyTemplateList::Evaluate(std::function<void(SKEEFixedString, float)> eval)
{
	if (size() > 0) {
		std::random_device rd;
		std::default_random_engine gen(rd());
		std::uniform_int_distribution<> rnd(0, size() - 1);

		auto & bodyTemplate = at(rnd(gen));
		return bodyTemplate->Evaluate(eval);
	}

	return 0;
}

UInt32 BodyGenDataTemplates::Evaluate(std::function<void(SKEEFixedString, float)> eval)
{
	UInt32 total = 0;
	for (auto & tempList : *this)
	{
		total += tempList.Evaluate(eval);
	}

	return total;
}

UInt32 BodyMorphInterface::Impl_EvaluateBodyMorphs(TESObjectREFR * actor)
{
	TESNPC * actorBase = DYNAMIC_CAST(actor->baseForm, TESForm, TESNPC);
	if (actorBase) {
		UInt64 gender = CALL_MEMBER_FN(actorBase, GetSex)();
		bool isFemale = gender == 1 ? true : false;
		BodyGenData::iterator morphSet = bodyGenData[gender].end();
		do {
			morphSet = bodyGenData[gender].find(actorBase);
			actorBase = actorBase->nextTemplate;
		} while (actorBase && morphSet == bodyGenData[gender].end());

		// Found a matching template
		if (morphSet != bodyGenData[gender].end()) {
			auto & templates = morphSet->second;
			UInt32 ret = templates->Evaluate([&](const SKEEFixedString & morphName, float value)
			{
				SetMorph(actor, morphName.c_str(), "RSMBodyGen", value);
			});

			_VMESSAGE("%s - Generated %d BodyMorphs for %s (%08X)", __FUNCTION__, ret, CALL_MEMBER_FN(actor, GetReferenceName)(), actor->formID);
			return ret;
		}
	}

	return 0;
}

void BodyMorphInterface::Impl_VisitStrings(std::function<void(SKEEFixedString)> functor)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	for (auto & i1 : actorMorphs.m_data) {
		for (auto & i2 : i1.second) {
			functor(*i2.first);
			for (auto & i3 : i2.second) {
				functor(*i3.first);
			}
		}
	}
}

void BodyMorphInterface::Impl_VisitActors(std::function<void(TESObjectREFR*)> functor)
{
	SimpleLocker locker(&actorMorphs.m_lock);
	for (auto & actor : actorMorphs.m_data) {
		TESObjectREFR * refr = (TESObjectREFR *)LookupFormByID(actor.first);
		if (refr) {
			functor(refr);
		}
	}
}

void BodyMorphInterface::ForEachMorphShapeCallback(std::function<void(IBodyMorphInterface::MorphShapeCallback)> func)
{
	shapeCallbacks.ForEach(func);
}

void BodyMorphInterface::OnAttach(TESObjectREFR * refr, TESObjectARMO * armor, TESObjectARMA * addon, NiAVObject * object, bool isFirstPerson, NiNode * skeleton, NiNode * root)
{
	if (HasMorphs(refr))
	{
		if (g_actorUpdateManager.isReverting())
		{
			g_actorUpdateManager.AddBodyUpdate(refr->formID);
		}
		else
		{
			ApplyVertexDiff(refr, object, true);
		}
	}
}

// Serialize Morph
void BodyMorph::Save(SKSESerializationInterface * intfc, UInt32 kVersion)
{
	intfc->OpenRecord('MRPV', kVersion);

	g_stringTable.WriteString(intfc, m_name);

	intfc->WriteRecordData(&m_value, sizeof(m_value));
}

bool BodyMorph::Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable)
{
	UInt32 type, length, version;
	bool error = false;

	m_value = 0.0;

	if(intfc->GetNextRecordInfo(&type, &version, &length))
	{
		switch (type)
		{
		case 'MRPV':
			{
				if (kVersion >= BodyMorphInterface::kSerializationVersion3)
				{
					m_name = StringTable::ReadString(intfc, stringTable);
				}
				else if (kVersion >= BodyMorphInterface::kSerializationVersion1)
				{
					UInt8 stringLength;
					if (!intfc->ReadRecordData(&stringLength, sizeof(stringLength)))
					{
						_ERROR("%s - Error loading body morph name length", __FUNCTION__);
						error = true;
						return error;
					}

					std::unique_ptr<char[]> stringName(new char[stringLength + 1]);
					if (!intfc->ReadRecordData(stringName.get(), stringLength)) {
						_ERROR("%s - Error loading body morph name", __FUNCTION__);
						error = true;
						return error;
					}
					stringName[stringLength] = 0;
					m_name = g_stringTable.GetString(stringName.get());
				}

				if (!intfc->ReadRecordData(&m_value, sizeof(m_value))) {
					_ERROR("%s - Error loading body morph value", __FUNCTION__);
					error = true;
					return error;
				}
			}
			break;
		default:
			{
				_ERROR("%s - Error loading unexpected chunk type %08X (%.4s)", __FUNCTION__, type, reinterpret_cast<char*>(&type));
				error = true;
				return error;
			}
		}
	}

	return error;
}

// Serialize Morph Set
void BodyMorphSet::Save(SKSESerializationInterface * intfc, UInt32 kVersion)
{
	intfc->OpenRecord('MRST', kVersion);

	UInt32 numMorphs = this->size();
	intfc->WriteRecordData(&numMorphs, sizeof(numMorphs));

#ifdef _DEBUG
	_DMESSAGE("%s - Saving %d morphs", __FUNCTION__, numMorphs);
#endif

	for(auto it = this->begin(); it != this->end(); ++it)
		const_cast<BodyMorph&>((*it)).Save(intfc, kVersion);
}

bool BodyMorphSet::Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable)
{
	UInt32 type, length, version;
	bool error = false;

	if(intfc->GetNextRecordInfo(&type, &version, &length))
	{
		switch (type)
		{
		case 'MRST':
			{
				// Override Count
				UInt32 numMorphs = 0;
				if (!intfc->ReadRecordData(&numMorphs, sizeof(numMorphs)))
				{
					_ERROR("%s - Error loading morph set count", __FUNCTION__);
					error = true;
					return error;
				}

				for (UInt32 i = 0; i < numMorphs; i++)
				{
					BodyMorph value;
					if (!value.Load(intfc, version, stringTable))
					{
						if(!value.m_name || *value.m_name == SKEEFixedString(""))
							continue;

#ifdef _DEBUG
						_DMESSAGE("%s - Loaded morph %s %f", __FUNCTION__, value.m_name.get()->c_str(), value.m_value);
#endif

						this->insert(value);
					}
					else
					{
						_ERROR("%s - Error loading morph value", __FUNCTION__);
						error = true;
						return error;
					}
				}

				break;
			}
		default:
			{
				_ERROR("%s - Error loading unexpected chunk type %08X (%.4s)", __FUNCTION__, type, reinterpret_cast<char*>(&type));
				error = true;
				return error;
			}
		}
	}

	return error;
}

// Serialize ActorMorphs
void ActorMorphs::Save(SKSESerializationInterface * intfc, UInt32 kVersion)
{
	SimpleLocker locker(&m_lock);
	for(auto & morph : m_data) {
		intfc->OpenRecord('MRPH', kVersion);

		// Key
		MorphKey formId = morph.first;
		intfc->WriteRecordData(&formId, sizeof(formId));

#ifdef _DEBUG
		_DMESSAGE("%s - Saving Morph form %08llX", __FUNCTION__, formId);
#endif

		// Value
		morph.second.Save(intfc, kVersion);
	}
}


// Serialize Morph Set
void BodyMorphData::Save(SKSESerializationInterface * intfc, UInt32 kVersion)
{
	intfc->OpenRecord('MRDT', kVersion);

	UInt32 numMorphs = this->size();
	intfc->WriteRecordData(&numMorphs, sizeof(numMorphs));

#ifdef _DEBUG
	_DMESSAGE("%s - Saving %d morphs", __FUNCTION__, numMorphs);
#endif

	for (auto & morph : *this)
	{
		g_stringTable.WriteString(intfc, morph.first);

		UInt32 numKeys = morph.second.size();
		intfc->WriteRecordData(&numKeys, sizeof(numKeys));

		for (auto & keys : morph.second)
		{
			g_stringTable.WriteString(intfc, keys.first);
			intfc->WriteRecordData(&keys.second, sizeof(keys.second));
		}
	}
}

bool BodyMorphData::Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable)
{
	UInt32 type, length, version;
	bool error = false;

	if (intfc->GetNextRecordInfo(&type, &version, &length))
	{
		switch (type)
		{
			case 'MRDT':
			{
				UInt32 numMorphs = 0;
				if (!intfc->ReadRecordData(&numMorphs, sizeof(numMorphs)))
				{
					_ERROR("%s - Error loading morph count", __FUNCTION__);
					error = true;
					return error;
				}

				for (UInt32 i = 0; i < numMorphs; i++)
				{
					auto morphName = StringTable::ReadString(intfc, stringTable);

					UInt32 numKeys = 0;
					if (!intfc->ReadRecordData(&numKeys, sizeof(numKeys)))
					{
						_ERROR("%s - Error loading morph key count", __FUNCTION__);
						error = true;
						return error;
					}

					absl::flat_hash_map<StringTableItem, float> pairs;
					for (UInt32 i = 0; i < numKeys; i++)
					{
						auto keyName = StringTable::ReadString(intfc, stringTable);

						float value = 0;
						if (!intfc->ReadRecordData(&value, sizeof(value))) {
							_ERROR("%s - Error loading body morph value", __FUNCTION__);
							error = true;
							return error;
						}

						if (!keyName)
							continue;

						// If the keys were mapped by mod name, skip them if they arent in load order
						std::string strKey(keyName->c_str());
						SKEEFixedString ext(strKey.substr(strKey.find_last_of(".") + 1).c_str());
						if (ext == SKEEFixedString("esp") || ext == SKEEFixedString("esm") || ext == SKEEFixedString("esl"))
						{
							if (!DataHandler::GetSingleton()->LookupModByName(keyName->c_str()))
								continue;
						}

						pairs.insert_or_assign(keyName, value);
					}

					if (morphName)
						insert_or_assign(morphName, pairs);
				}

				break;
			}
			default:
			{
				_ERROR("%s - Error loading unexpected chunk type %08X (%.4s)", __FUNCTION__, type, reinterpret_cast<char*>(&type));
				error = true;
				return error;
			}
		}
	}

	return error;
}

bool ActorMorphs::Load(SKSESerializationInterface * intfc, UInt32 kVersion, const StringIdMap & stringTable)
{
	bool error = false;

	UInt64 handle;
	MorphKey formId;
	// Key
	if (kVersion >= BodyMorphInterface::kSerializationVersion3)
	{
		if (!intfc->ReadRecordData(&formId, sizeof(formId)))
		{
			_ERROR("%s - Error loading MorphSet key", __FUNCTION__);
			error = true;
			return error;
		}
	}
	else
	{
		if (!intfc->ReadRecordData(&handle, sizeof(handle)))
		{
			_ERROR("%s - Error loading MorphSet key", __FUNCTION__);
			error = true;
			return error;
		}
	}
	

	BodyMorphSet morphSet;
	BodyMorphData morphMap;

	if (kVersion >= BodyMorphInterface::kSerializationVersion2)
	{
		if (morphMap.Load(intfc, kVersion, stringTable))
		{
			_ERROR("%s - Error loading MorphMap", __FUNCTION__);
			error = true;
			return error;
		}
	}
	else if (kVersion >= BodyMorphInterface::kSerializationVersion1)
	{
		if (morphSet.Load(intfc, kVersion, stringTable))
		{
			_ERROR("%s - Error loading MorphSet", __FUNCTION__);
			error = true;
			return error;
		}
	}

	if (!morphSet.empty())
	{
		for (auto & morph : morphSet)
		{
			morphMap[morph.m_name][g_stringTable.GetString("RSMLegacy")] = morph.m_value;
		}
	}

	MorphKey newFormId = 0;
	if (kVersion >= BodyMorphInterface::kSerializationVersion3)
	{
		// Skip if handle is no longer valid.
		if (!ResolveAnyForm(intfc, formId, &newFormId))
			return false;
	}
	else
	{
		// Skip if handle is no longer valid.
		UInt64 newHandle = 0;
		if (!intfc->ResolveHandle(handle, &newHandle))
			return false;

		newFormId = newHandle & 0xFFFFFFFF;
	}
	
	if(morphMap.empty())
		return false;

	auto form = LookupFormByID(newFormId);
	if (!form) {
		_WARNING("%s - Discarding body morphs for (%08llX) form is invalid", __FUNCTION__, newFormId);
		return false;
	}
	else if (form->formType != kFormType_Reference && form->formType != kFormType_Character) {
		_WARNING("%s - Discarding body morphs for (%08llX) form is not an actor or reference (%d)", __FUNCTION__, newFormId, form->formType);
		return false;
	}
	else if ((form->flags & TESForm::kFlagIsDeleted) == TESForm::kFlagIsDeleted) {
		_WARNING("%s - Discarding body morphs for (%08llX) form is deleted", __FUNCTION__, newFormId);
		return false;
	}

	if (g_enableBodyMorph)
	{

		Lock();
		m_data.insert_or_assign(newFormId, morphMap);
		Release();

#ifdef _DEBUG
		_DMESSAGE("%s - Loaded MorphSet Handle %08llX actor (%s)", __FUNCTION__, newFormId, CALL_MEMBER_FN(static_cast<TESObjectREFR*>(form), GetReferenceName)());
#endif
		g_actorUpdateManager.AddBodyUpdate(newFormId);
		
	}
	return error;
}

// Serialize Morphs
void BodyMorphInterface::Save(SKSESerializationInterface * intfc, UInt32 kVersion)
{
	actorMorphs.Save(intfc, kVersion);
}

bool BodyMorphInterface::Load(SKSESerializationInterface * intfc, UInt32 kVersion, const absl::flat_hash_map<UInt32, StringTableItem> & stringTable)
{
	return actorMorphs.Load(intfc, kVersion, stringTable);
}

void MorphShapeCallbacks::AddCallback(IBodyMorphInterface::MorphShapeCallback cb, skee_u64 order)
{
	SimpleLocker locker(&m_lock);
	m_data.insert(MorphShapeCallbackItem{ cb, order });
}

void MorphShapeCallbacks::ForEach(std::function<void(IBodyMorphInterface::MorphShapeCallback)> func)
{
	SimpleLocker locker(&m_lock);
	for (auto& item : m_data)
	{
		func(item.cb);
	}
}
