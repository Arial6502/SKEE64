#pragma once

#include "shape.hpp"
#include "kd_matcher.hpp"

#include <vector>
#include <functional>
#include <unordered_set>

#include "skse64/NiTypes.h"

class BSGeometry;
class NiSkinPartition;

class NormalApplicator
{
public:
	// Works on the first partition's vertices unless given a block of its own
	NormalApplicator(NiPointer<BSGeometry> _geometry, NiPointer<NiSkinPartition> _skinPartition, UInt8* _vertexBlock = nullptr);

	void Apply();

	void RecalcNormals(UInt32 numTriangles, Morpher::Triangle* triangles, const bool smooth = true, const float smoothThres = 60.0f);
	void CalcTangentSpace(UInt32 numTriangles, Morpher::Triangle * triangles);

protected:
	NiPointer<BSGeometry> geometry;
	NiPointer<NiSkinPartition> skinPartition;
	UInt8* vertexBlock;
	absl::flat_hash_set<UInt16> lockedVertices;
	std::vector<Morpher::Vector3> rawVertices;
	std::vector<Morpher::Vector3> rawNormals;
	std::vector<Morpher::Vector2> rawUV;
	std::vector<Morpher::Vector3> rawTangents;
	std::vector<Morpher::Vector3> rawBitangents;
};
