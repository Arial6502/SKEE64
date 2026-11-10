#pragma once

#include "skse64/GameTypes.h"
#include "skse64/GameThreads.h"

#include "skse64/NiGeometry.h"
#include "skse64/NiNodes.h"
#include "skse64/NiTypes.h"

#include "StringTable.h"
#include "OverrideVariant.h"

class BSGeometry;
class BSLightingShaderMaterial;

class NIOVTaskUpdateTexture : public TaskDelegate
{
public:
	NIOVTaskUpdateTexture(NiPointer<BSGeometry> geometry, UInt32 index, StringTableItem texture) : m_geometry(geometry), m_index(index), m_texture(texture) { }

	virtual void Run();
	virtual void Dispose() { delete this; }

	// Loads the texture on a worker thread, then swaps it in on the main thread
	static void Queue(NiPointer<BSGeometry> geometry, UInt32 index, StringTableItem texture);

	NiPointer<BSGeometry> m_geometry;
	UInt32			m_index;
	StringTableItem	m_texture;
	NiPointer<NiTexture> m_loaded;
	bool			m_preloaded = false;
};

class NIOVTaskUpdateWorldData : public TaskDelegate
{
public:
	NIOVTaskUpdateWorldData(NiPointer<NiAVObject> object) : m_object(object) { }

	virtual void Run();
	virtual void Dispose() { delete this; }

	NiPointer<NiAVObject> m_object;
};


class NIOVTaskMoveNode : public TaskDelegate
{
public:
	NIOVTaskMoveNode(NiPointer<NiNode> destination, NiPointer<NiAVObject> object) : m_object(object), m_destination(destination) { }

	virtual void Run();
	virtual void Dispose() { delete this; }

	NiPointer<NiAVObject> m_object;
	NiPointer<NiNode> m_destination;
};

void GetShaderProperty(NiAVObject * node, OverrideVariant * value);
void SetShaderProperty(NiAVObject * node, OverrideVariant * value, bool immediate);

class NIOVTaskSetShaderProperty : public TaskDelegate
{
public:
	NIOVTaskSetShaderProperty(NiAVObject* node, const OverrideVariant& variant) : m_object(node), m_variant(variant) { }

	virtual void Run()
	{
		// A deferred texture is read from disk off the main thread, every other key applies now
		SetShaderProperty(m_object, &m_variant, m_variant.key != OverrideVariant::kParam_ShaderTexture);
	}

	virtual void Dispose() { delete this; }

protected:
	NiPointer<NiAVObject> m_object;
	OverrideVariant m_variant;
};


SKEEFixedString GetSanitizedPath(const SKEEFixedString & path);
NiTexturePtr * GetTextureFromIndex(BSLightingShaderMaterial* material, UInt32 index);

bool IsSameTexturePath(const char * a, const char * b);

// Points one texture slot of the material at path, loading only that texture. Returns whether the slot changed,
// the caller then calls InitializeShader once to fill empty slots with defaults
bool LoadMaterialTexture(BSLightingShaderMaterial * material, UInt32 index, const char * path);

void DumpNodeChildren(NiAVObject * node);