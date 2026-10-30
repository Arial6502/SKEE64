#include "MorphKernel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <immintrin.h>
#include <intrin.h>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <half.hpp>

#include "shape.hpp"
#include "kd_matcher.hpp"

namespace MorphKernel
{
	static constexpr uint32_t kMaxWidth = 8;

	// MSVC allows AVX intrinsics without /arch, so one source file serves both widths and the choice is made at runtime
	static bool DetectAVX2()
	{
		int info[4];
		__cpuid(info, 1);
		const bool osSavesYmm = (info[2] & (1 << 27)) != 0 && (info[2] & (1 << 28)) != 0 && (_xgetbv(0) & 6) == 6;
		__cpuidex(info, 7, 0);
		return osSavesYmm && (info[1] & (1 << 5)) != 0;
	}

	bool UsesAVX2()
	{
		static const bool supported = DetectAVX2();
		return supported;
	}

	struct Sse
	{
		static constexpr uint32_t kWidth = 4;
		__m128 v;

		static Sse Load(const float* p) { return { _mm_load_ps(p) }; }
		static Sse Set(float value) { return { _mm_set1_ps(value) }; }
		static Sse Zero() { return { _mm_setzero_ps() }; }
		void Store(float* p) const { _mm_store_ps(p, v); }
		void StoreBytes(int32_t* p) const { _mm_store_si128(reinterpret_cast<__m128i*>(p), _mm_cvttps_epi32(v)); }
		static Sse Gather(const float* values, const int32_t* indices) { return { _mm_set_ps(values[indices[9]], values[indices[6]], values[indices[3]], values[indices[0]]) }; }

		friend Sse operator+(Sse a, Sse b) { return { _mm_add_ps(a.v, b.v) }; }
		friend Sse operator-(Sse a, Sse b) { return { _mm_sub_ps(a.v, b.v) }; }
		friend Sse operator*(Sse a, Sse b) { return { _mm_mul_ps(a.v, b.v) }; }
		friend Sse operator/(Sse a, Sse b) { return { _mm_div_ps(a.v, b.v) }; }
		static Sse Sqrt(Sse a) { return { _mm_sqrt_ps(a.v) }; }
		static Sse IsZero(Sse a) { return { _mm_cmpeq_ps(a.v, _mm_setzero_ps()) }; }
		static Sse And(Sse a, Sse b) { return { _mm_and_ps(a.v, b.v) }; }
		static Sse Or(Sse a, Sse b) { return { _mm_or_ps(a.v, b.v) }; }
		static Sse Blend(Sse a, Sse b, Sse mask) { return { _mm_blendv_ps(a.v, b.v, mask.v) }; }
		static bool Any(Sse mask) { return _mm_movemask_ps(mask.v) != 0; }
		static void Finish() {}

		// Lane by lane x y z records, four per vector here
		static void Interleave(Sse x, Sse y, Sse z, __m128* out)
		{
			__m128 r0 = x.v, r1 = y.v, r2 = z.v, r3 = _mm_setzero_ps();
			_MM_TRANSPOSE4_PS(r0, r1, r2, r3);
			out[0] = r0; out[1] = r1; out[2] = r2; out[3] = r3;
		}

		// The reverse, for records every three __m128 apart
		static void Deinterleave(const __m128* records, Sse& x, Sse& y, Sse& z)
		{
			__m128 r0 = records[0], r1 = records[3], r2 = records[6], r3 = records[9];
			_MM_TRANSPOSE4_PS(r0, r1, r2, r3);
			x.v = r0; y.v = r1; z.v = r2;
		}
	};

	struct Avx2
	{
		static constexpr uint32_t kWidth = 8;
		__m256 v;

		static Avx2 Load(const float* p) { return { _mm256_load_ps(p) }; }
		static Avx2 Set(float value) { return { _mm256_set1_ps(value) }; }
		static Avx2 Zero() { return { _mm256_setzero_ps() }; }
		void Store(float* p) const { _mm256_store_ps(p, v); }
		void StoreBytes(int32_t* p) const { _mm256_store_si256(reinterpret_cast<__m256i*>(p), _mm256_cvttps_epi32(v)); }

		// Every third index of a triangle run, starting at this corner
		static Avx2 Gather(const float* values, const int32_t* indices)
		{
			const __m256i offsets = _mm256_set_epi32(indices[21], indices[18], indices[15], indices[12], indices[9], indices[6], indices[3], indices[0]);
			return { _mm256_i32gather_ps(values, offsets, 4) };
		}

		friend Avx2 operator+(Avx2 a, Avx2 b) { return { _mm256_add_ps(a.v, b.v) }; }
		friend Avx2 operator-(Avx2 a, Avx2 b) { return { _mm256_sub_ps(a.v, b.v) }; }
		friend Avx2 operator*(Avx2 a, Avx2 b) { return { _mm256_mul_ps(a.v, b.v) }; }
		friend Avx2 operator/(Avx2 a, Avx2 b) { return { _mm256_div_ps(a.v, b.v) }; }
		static Avx2 Sqrt(Avx2 a) { return { _mm256_sqrt_ps(a.v) }; }
		static Avx2 IsZero(Avx2 a) { return { _mm256_cmp_ps(a.v, _mm256_setzero_ps(), _CMP_EQ_OQ) }; }
		static Avx2 And(Avx2 a, Avx2 b) { return { _mm256_and_ps(a.v, b.v) }; }
		static Avx2 Or(Avx2 a, Avx2 b) { return { _mm256_or_ps(a.v, b.v) }; }
		static Avx2 Blend(Avx2 a, Avx2 b, Avx2 mask) { return { _mm256_blendv_ps(a.v, b.v, mask.v) }; }
		static bool Any(Avx2 mask) { return _mm256_movemask_ps(mask.v) != 0; }
		static void Finish() { _mm256_zeroupper(); }

		static void Interleave(Avx2 x, Avx2 y, Avx2 z, __m128* out)
		{
			const __m256 zero = _mm256_setzero_ps();
			const __m256 xyLow = _mm256_unpacklo_ps(x.v, y.v);
			const __m256 xyHigh = _mm256_unpackhi_ps(x.v, y.v);
			const __m256 zLow = _mm256_unpacklo_ps(z.v, zero);
			const __m256 zHigh = _mm256_unpackhi_ps(z.v, zero);
			const __m256 r0 = _mm256_shuffle_ps(xyLow, zLow, 0x44);
			const __m256 r1 = _mm256_shuffle_ps(xyLow, zLow, 0xEE);
			const __m256 r2 = _mm256_shuffle_ps(xyHigh, zHigh, 0x44);
			const __m256 r3 = _mm256_shuffle_ps(xyHigh, zHigh, 0xEE);
			out[0] = _mm256_castps256_ps128(r0);
			out[1] = _mm256_castps256_ps128(r1);
			out[2] = _mm256_castps256_ps128(r2);
			out[3] = _mm256_castps256_ps128(r3);
			out[4] = _mm256_extractf128_ps(r0, 1);
			out[5] = _mm256_extractf128_ps(r1, 1);
			out[6] = _mm256_extractf128_ps(r2, 1);
			out[7] = _mm256_extractf128_ps(r3, 1);
		}

		static void Deinterleave(const __m128* records, Avx2& x, Avx2& y, Avx2& z)
		{
			const __m256 r0 = _mm256_set_m128(records[12], records[0]);
			const __m256 r1 = _mm256_set_m128(records[15], records[3]);
			const __m256 r2 = _mm256_set_m128(records[18], records[6]);
			const __m256 r3 = _mm256_set_m128(records[21], records[9]);
			const __m256 low01 = _mm256_unpacklo_ps(r0, r1);
			const __m256 high01 = _mm256_unpackhi_ps(r0, r1);
			const __m256 low23 = _mm256_unpacklo_ps(r2, r3);
			const __m256 high23 = _mm256_unpackhi_ps(r2, r3);
			x.v = _mm256_shuffle_ps(low01, low23, 0x44);
			y.v = _mm256_shuffle_ps(low01, low23, 0xEE);
			z.v = _mm256_shuffle_ps(high01, high23, 0x44);
		}
	};

	// What a vertex collects from its triangles. Kept together so each corner of a triangle is one cache line
	// instead of nine, which is what lets the wider vectors pay off.
	struct Frame
	{
		__m128 normal;
		__m128 tangent;
		__m128 bitangent;
	};

	struct Channels
	{
		float* px; float* py; float* pz;
		Frame* frames;
	};

	static uint32_t Padded(uint32_t count)
	{
		return (count + kMaxWidth - 1) & ~(kMaxWidth - 1);
	}

	static Channels Prepare(Scratch& scratch, uint32_t vertexCount)
	{
		const uint32_t width = Padded(vertexCount);
		scratch.values.resize(width * (3 + sizeof(Frame) / sizeof(float)) + 8);

		// std::vector<float> is only 16 byte aligned on its own
		float* base = reinterpret_cast<float*>((reinterpret_cast<uintptr_t>(scratch.values.data()) + 31) & ~uintptr_t(31));

		Channels c;
		c.px = base;
		c.py = base + width;
		c.pz = base + width * 2;
		c.frames = reinterpret_cast<Frame*>(base + width * 3);
		memset(c.frames, 0, sizeof(Frame) * width);
		return c;
	}

	// Length of each lane, with a length of zero treated as one like Vector3::Normalize
	template<class V>
	static inline V SafeLength(V x, V y, V z)
	{
		const V length = V::Sqrt(x * x + y * y + z * z);
		return V::Blend(length, V::Set(1.0f), V::IsZero(length));
	}

	Topology::Topology(const uint8_t* vertices, uint32_t vertexCount, const VertexLayout& layout, const std::vector<TriangleRun>& triangles, const std::vector<uint16_t>& lockedVertices)
		: m_layout(layout)
		, m_vertexCount(vertexCount)
		, m_sourceLocked(lockedVertices)
	{
		for (const auto& run : triangles)
			m_sourceTriangles.insert(m_sourceTriangles.end(), run.indices, run.indices + run.count * 3);

		m_sourceUVs.resize(vertexCount);
		for (uint32_t i = 0; i < vertexCount; i++)
			memcpy(&m_sourceUVs[i], vertices + i * layout.stride + layout.uvOffset, sizeof(uint32_t));

		const uint32_t triangleCount = static_cast<uint32_t>(m_sourceTriangles.size() / 3);
		const uint16_t* source = m_sourceTriangles.data();

		m_triangleCount = Padded(triangleCount);
		m_indices.assign(m_triangleCount * 3, 0);
		for (uint32_t i = 0; i < triangleCount * 3; i++)
			m_indices[i] = source[i];

		// Padded so a full vector can always be loaded, the extra triangles have no area and no UV terms
		m_tangentTerms.assign(m_triangleCount * 4 + 8, 0.0f);
		float* sa = reinterpret_cast<float*>((reinterpret_cast<uintptr_t>(m_tangentTerms.data()) + 31) & ~uintptr_t(31));
		float* sb = sa + m_triangleCount;
		float* ta = sb + m_triangleCount;
		float* tb = ta + m_triangleCount;

		std::vector<float> u(vertexCount), v(vertexCount);
		std::vector<Morpher::Vector3> positions(vertexCount);
		for (uint32_t i = 0; i < vertexCount; i++)
		{
			const uint8_t* vertex = vertices + i * layout.stride;
			const float* position = reinterpret_cast<const float*>(vertex);
			positions[i].x = position[0] * -0.1f;
			positions[i].z = position[1] * 0.1f;
			positions[i].y = position[2] * 0.1f;

			half_float::half halves[2];
			memcpy(halves, vertex + layout.uvOffset, sizeof(halves));
			u[i] = halves[0];
			v[i] = halves[1];
		}

		for (uint32_t t = 0; t < triangleCount; t++)
		{
			const uint16_t a = source[t * 3 + 0];
			const uint16_t b = source[t * 3 + 1];
			const uint16_t c = source[t * 3 + 2];

			const float s1 = u[b] - u[a];
			const float s2 = u[c] - u[a];
			const float t1 = v[b] - v[a];
			const float t2 = v[c] - v[a];
			const float r = (s1 * t2 - s2 * t1) >= 0.0f ? 1.0f : -1.0f;

			sa[t] = t2 * r;
			sb[t] = -t1 * r;
			ta[t] = -s2 * r;
			tb[t] = s1 * r;
		}

		kd_matcher matcher(positions.data(), vertexCount);
		m_seams.reserve(matcher.matches.size());
		for (auto& match : matcher.matches)
			m_seams.emplace_back(static_cast<uint16_t>(match.first.second), static_cast<uint16_t>(match.second.second));

		for (const auto& seam : m_seams)
		{
			m_seamVertices.push_back(seam.first);
			m_seamVertices.push_back(seam.second);
		}
		std::sort(m_seamVertices.begin(), m_seamVertices.end());
		m_seamVertices.erase(std::unique(m_seamVertices.begin(), m_seamVertices.end()), m_seamVertices.end());

		if (!lockedVertices.empty())
		{
			m_locked.assign(Padded(vertexCount), 0);
			for (uint16_t vertex : lockedVertices)
			{
				if (vertex < vertexCount)
					m_locked[vertex] = 1;
			}
		}
	}

	bool Topology::Matches(const uint8_t* vertices, uint32_t vertexCount, const VertexLayout& layout, const std::vector<TriangleRun>& triangles, const std::vector<uint16_t>& lockedVertices) const
	{
		if (vertexCount != m_vertexCount || memcmp(&layout, &m_layout, sizeof(layout)) != 0 || lockedVertices != m_sourceLocked)
			return false;

		size_t offset = 0;
		for (const auto& run : triangles)
		{
			const size_t count = size_t(run.count) * 3;
			if (offset + count > m_sourceTriangles.size() || memcmp(run.indices, m_sourceTriangles.data() + offset, count * sizeof(uint16_t)) != 0)
				return false;
			offset += count;
		}
		if (offset != m_sourceTriangles.size())
			return false;

		for (uint32_t i = 0; i < vertexCount; i++)
		{
			uint32_t uv;
			memcpy(&uv, vertices + i * layout.stride + layout.uvOffset, sizeof(uv));
			if (uv != m_sourceUVs[i])
				return false;
		}
		return true;
	}

	template<class V>
	static void RecalculateImpl(uint8_t* vertices, Scratch& scratch, uint32_t vertexCount, uint32_t triangleCount, const VertexLayout& layout, const int32_t* allIndices, const float* terms, const std::vector<std::pair<uint16_t, uint16_t>>& seams, const std::vector<uint16_t>& seamVertices, const std::vector<uint8_t>& locked)
	{
		const Channels c = Prepare(scratch, vertexCount);
		const uint32_t stride = layout.stride;
		Frame* frames = c.frames;

		for (uint32_t i = 0; i < vertexCount; i++)
		{
			const float* position = reinterpret_cast<const float*>(vertices + i * stride);
			c.px[i] = position[0];
			c.py[i] = position[1];
			c.pz[i] = position[2];
		}

		const float* sa = terms;
		const float* sb = sa + triangleCount;
		const float* ta = sb + triangleCount;
		const float* tb = ta + triangleCount;

		__m128 normals[kMaxWidth], tangents[kMaxWidth], bitangents[kMaxWidth];
		for (uint32_t t = 0; t < triangleCount; t += V::kWidth)
		{
			const int32_t* indices = allIndices + t * 3;

			const V ax = V::Gather(c.px, indices);
			const V ay = V::Gather(c.py, indices);
			const V az = V::Gather(c.pz, indices);

			const V e1x = V::Gather(c.px, indices + 1) - ax;
			const V e1y = V::Gather(c.py, indices + 1) - ay;
			const V e1z = V::Gather(c.pz, indices + 1) - az;
			const V e2x = V::Gather(c.px, indices + 2) - ax;
			const V e2y = V::Gather(c.py, indices + 2) - ay;
			const V e2z = V::Gather(c.pz, indices + 2) - az;

			// Normals stay area weighted, so they aren't normalized until they've all been added up
			V::Interleave(e1y * e2z - e1z * e2y, e1z * e2x - e1x * e2z, e1x * e2y - e1y * e2x, normals);

			const V vsa = V::Load(sa + t);
			const V vsb = V::Load(sb + t);
			const V vta = V::Load(ta + t);
			const V vtb = V::Load(tb + t);

			const V sx = vsa * e1x + vsb * e2x;
			const V sy = vsa * e1y + vsb * e2y;
			const V sz = vsa * e1z + vsb * e2z;
			const V tx = vta * e1x + vtb * e2x;
			const V ty = vta * e1y + vtb * e2y;
			const V tz = vta * e1z + vtb * e2z;

			const V sLength = SafeLength(sx, sy, sz);
			const V tLength = SafeLength(tx, ty, tz);

			V::Interleave(tx / tLength, ty / tLength, tz / tLength, tangents);
			V::Interleave(sx / sLength, sy / sLength, sz / sLength, bitangents);

			for (uint32_t lane = 0; lane < V::kWidth; lane++)
			{
				for (uint32_t corner = 0; corner < 3; corner++)
				{
					Frame& frame = frames[indices[lane * 3 + corner]];
					frame.normal = _mm_add_ps(frame.normal, normals[lane]);
					frame.tangent = _mm_add_ps(frame.tangent, tangents[lane]);
					frame.bitangent = _mm_add_ps(frame.bitangent, bitangents[lane]);
				}
			}
		}

		// Seams weigh both sides equally, every other normal is only normalized once it's final
		for (uint32_t vertex : seamVertices)
		{
			const float length = _mm_cvtss_f32(_mm_sqrt_ss(_mm_dp_ps(frames[vertex].normal, frames[vertex].normal, 0x71)));
			if (length != 0.0f)
				frames[vertex].normal = _mm_div_ps(frames[vertex].normal, _mm_set1_ps(length));
		}

		// Smooth across seams closer than 60 degrees. Comparing squares avoids an acos per pair.
		for (const auto& seam : seams)
		{
			const uint32_t a = seam.first;
			const uint32_t b = seam.second;

			// A morph can pull a seam apart
			if (fabs(c.px[a] - c.px[b]) * 0.1f >= EPSILON || fabs(c.py[a] - c.py[b]) * 0.1f >= EPSILON || fabs(c.pz[a] - c.pz[b]) * 0.1f >= EPSILON)
				continue;

			const __m128 na = frames[a].normal;
			const __m128 nb = frames[b].normal;
			const float dot = _mm_cvtss_f32(_mm_dp_ps(na, nb, 0x71));
			if (dot <= 0.0f)
				continue;

			const float lengthA = _mm_cvtss_f32(_mm_dp_ps(na, na, 0x71));
			const float lengthB = _mm_cvtss_f32(_mm_dp_ps(nb, nb, 0x71));
			if (dot * dot > 0.25f * lengthA * lengthB)
				frames[a].normal = frames[b].normal = _mm_add_ps(na, nb);
		}

		const V one = V::Set(1.0f);
		const V half = V::Set(0.5f);
		const V scale = V::Set(127.5f);
		const auto toByte = [&](V value, int32_t* out) { ((value + one) * scale + half).StoreBytes(out); };

		alignas(32) int32_t bytes[8][kMaxWidth];
		alignas(32) float bitangentX[kMaxWidth];
		for (uint32_t i = 0; i < vertexCount; i += V::kWidth)
		{
			const __m128* record = reinterpret_cast<const __m128*>(frames + i);

			V nx, ny, nz, tx, ty, tz, bx, by, bz;
			V::Deinterleave(record, nx, ny, nz);
			V::Deinterleave(record + 1, tx, ty, tz);
			V::Deinterleave(record + 2, bx, by, bz);

			{
				const V nLength = SafeLength(nx, ny, nz);
				nx = nx / nLength;
				ny = ny / nLength;
				nz = nz / nLength;
			}

			const V tangentZero = V::And(V::And(V::IsZero(tx), V::IsZero(ty)), V::IsZero(tz));
			const V bitangentZero = V::And(V::And(V::IsZero(bx), V::IsZero(by)), V::IsZero(bz));
			const V degenerate = V::Or(tangentZero, bitangentZero);

			// Keep the tangent in the normal's plane
			V length = SafeLength(tx, ty, tz);
			tx = tx / length;
			ty = ty / length;
			tz = tz / length;

			V dot = nx * tx + ny * ty + nz * tz;
			tx = tx - nx * dot;
			ty = ty - ny * dot;
			tz = tz - nz * dot;
			length = SafeLength(tx, ty, tz);
			tx = tx / length;
			ty = ty / length;
			tz = tz / length;

			// Then the bitangent against both
			length = SafeLength(bx, by, bz);
			bx = bx / length;
			by = by / length;
			bz = bz / length;

			dot = nx * bx + ny * by + nz * bz;
			bx = bx - nx * dot;
			by = by - ny * dot;
			bz = bz - nz * dot;

			dot = tx * bx + ty * by + tz * bz;
			bx = bx - tx * dot;
			by = by - ty * dot;
			bz = bz - tz * dot;

			length = SafeLength(bx, by, bz);
			bx = bx / length;
			by = by / length;
			bz = bz / length;

			// No usable UV direction, so build a frame from the normal alone
			if (V::Any(degenerate))
			{
				tx = V::Blend(tx, ny, degenerate);
				ty = V::Blend(ty, nz, degenerate);
				tz = V::Blend(tz, nx, degenerate);
				bx = V::Blend(bx, ny * nx - nz * nz, degenerate);
				by = V::Blend(by, nz * ny - nx * nx, degenerate);
				bz = V::Blend(bz, nx * nz - ny * ny, degenerate);
			}

			bx.Store(bitangentX);
			toByte(nx, bytes[0]);
			toByte(ny, bytes[1]);
			toByte(nz, bytes[2]);
			toByte(by, bytes[3]);
			toByte(tx, bytes[4]);
			toByte(ty, bytes[5]);
			toByte(tz, bytes[6]);
			toByte(bz, bytes[7]);

			const uint32_t end = (i + V::kWidth < vertexCount) ? V::kWidth : vertexCount - i;
			for (uint32_t lane = 0; lane < end; lane++)
			{
				const uint32_t vertex = i + lane;
				if (!locked.empty() && locked[vertex])
					continue;

				uint8_t* block = vertices + vertex * stride;
				memcpy(block + 12, &bitangentX[lane], sizeof(float));

				uint8_t* normal = block + layout.normalOffset;
				normal[0] = static_cast<uint8_t>(bytes[0][lane]);
				normal[1] = static_cast<uint8_t>(bytes[1][lane]);
				normal[2] = static_cast<uint8_t>(bytes[2][lane]);
				normal[3] = static_cast<uint8_t>(bytes[3][lane]);

				uint8_t* tangent = block + layout.tangentOffset;
				tangent[0] = static_cast<uint8_t>(bytes[4][lane]);
				tangent[1] = static_cast<uint8_t>(bytes[5][lane]);
				tangent[2] = static_cast<uint8_t>(bytes[6][lane]);
				tangent[3] = static_cast<uint8_t>(bytes[7][lane]);
			}
		}

		V::Finish();
	}

	void Topology::Recalculate(uint8_t* vertices, Scratch& scratch) const
	{
		const float* terms = reinterpret_cast<const float*>((reinterpret_cast<uintptr_t>(m_tangentTerms.data()) + 31) & ~uintptr_t(31));
		if (UsesAVX2())
			RecalculateImpl<Avx2>(vertices, scratch, m_vertexCount, m_triangleCount, m_layout, m_indices.data(), terms, m_seams, m_seamVertices, m_locked);
		else
			RecalculateImpl<Sse>(vertices, scratch, m_vertexCount, m_triangleCount, m_layout, m_indices.data(), terms, m_seams, m_seamVertices, m_locked);
	}
}
