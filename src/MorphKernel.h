#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// Normal and tangent recalculation for morphed shapes. Everything that doesn't depend on the morph is worked
// out once per mesh: which vertices share a seam, and each triangle's UV terms. Has no game dependencies, so it
// can be benchmarked and compared against NormalApplicator outside of the game.
namespace MorphKernel
{
	// The vertex layout NormalApplicator reads: position and bitangent x, half float UVs, normal, tangent
	struct VertexLayout
	{
		uint32_t	stride;
		uint32_t	uvOffset;
		uint32_t	normalOffset;
		uint32_t	tangentOffset;
	};

	struct Scratch
	{
		std::vector<float> values;
	};

	bool UsesAVX2();

	// Skin partitions each hold a run of triangles, all indexing the same vertex block
	struct TriangleRun
	{
		const uint16_t*	indices;
		uint32_t		count;
	};

	class Topology
	{
	public:
		// Vertices is the unmorphed block
		Topology(const uint8_t* vertices, uint32_t vertexCount, const VertexLayout& layout, const std::vector<TriangleRun>& triangles, const std::vector<uint16_t>& lockedVertices);

		// Whether this was built from the same mesh. Positions are left out, they differ with actor weight.
		bool Matches(const uint8_t* vertices, uint32_t vertexCount, const VertexLayout& layout, const std::vector<TriangleRun>& triangles, const std::vector<uint16_t>& lockedVertices) const;

		// Rewrites the normals, tangents and bitangents in a morphed vertex block
		void Recalculate(uint8_t* vertices, Scratch& scratch) const;

		uint32_t VertexCount() const { return m_vertexCount; }

	private:
		VertexLayout	m_layout;
		uint32_t		m_vertexCount;
		uint32_t		m_triangleCount;	// Rounded up to a full vector with empty triangles

		std::vector<int32_t>	m_indices;	// a b c, one triangle after the other
		std::vector<float>		m_tangentTerms;	// Four runs of m_triangleCount floats
		std::vector<std::pair<uint16_t, uint16_t>> m_seams;
		std::vector<uint16_t>	m_seamVertices;
		std::vector<uint8_t>	m_locked;

		std::vector<uint16_t>	m_sourceTriangles;
		std::vector<uint32_t>	m_sourceUVs;
		std::vector<uint16_t>	m_sourceLocked;
	};
}
