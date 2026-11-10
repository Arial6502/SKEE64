#include "MorphCollision.h"

#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <tuple>
#include <utility>

namespace MorphCollision
{
	namespace
	{
		struct Vec
		{
			float x, y, z;
		};

		Vec operator+(Vec a, Vec b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
		Vec operator-(Vec a, Vec b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
		Vec operator*(Vec a, float s) { return { a.x * s, a.y * s, a.z * s }; }
		float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
		Vec Cross(Vec a, Vec b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

		Vec Load(const uint8_t* vertices, uint32_t stride, uint32_t index)
		{
			Vec v;
			memcpy(&v, vertices + size_t(stride) * index, sizeof(v));
			return v;
		}

		Vec Load(const float* positions, uint32_t index)
		{
			return { positions[index * 3], positions[index * 3 + 1], positions[index * 3 + 2] };
		}

		Vec Load(const std::vector<float>& positions, uint32_t index)
		{
			return Load(positions.data(), index);
		}

		// Ericson, Real-Time Collision Detection 5.1.5, giving the weights of b and c
		void ClosestOnTriangle(Vec p, Vec a, Vec b, Vec c, float& u, float& v)
		{
			const Vec ab = b - a, ac = c - a, ap = p - a;
			const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
			if (d1 <= 0.0f && d2 <= 0.0f) { u = 0.0f; v = 0.0f; return; }

			const Vec bp = p - b;
			const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
			if (d3 >= 0.0f && d4 <= d3) { u = 1.0f; v = 0.0f; return; }

			const float vc = d1 * d4 - d3 * d2;
			if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) { u = d1 / (d1 - d3); v = 0.0f; return; }

			const Vec cp = p - c;
			const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
			if (d6 >= 0.0f && d5 <= d6) { u = 0.0f; v = 1.0f; return; }

			const float vb = d5 * d2 - d1 * d6;
			if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) { u = 0.0f; v = d2 / (d2 - d6); return; }

			const float va = d3 * d6 - d5 * d4;
			if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f)
			{
				const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
				u = 1.0f - w;
				v = w;
				return;
			}

			const float denom = 1.0f / (va + vb + vc);
			u = vb * denom;
			v = vc * denom;
		}

		uint16_t Unorm(float x)
		{
			return uint16_t((std::clamp)(x, 0.0f, 1.0f) * 65535.0f + 0.5f);
		}

		// Skin vertices bucketed into cubes over their bounds
		class Grid
		{
		public:
			Grid(const std::vector<float>& positions, const std::vector<uint8_t>& used, float cell) : m_cell(cell)
			{
				const uint32_t count = uint32_t(used.size());
				for (uint32_t i = 0; i < count; ++i)
				{
					if (!used[i])
						continue;

					const Vec p = Load(positions, i);
					m_low = { (std::min)(m_low.x, p.x), (std::min)(m_low.y, p.y), (std::min)(m_low.z, p.z) };
					m_high = { (std::max)(m_high.x, p.x), (std::max)(m_high.y, p.y), (std::max)(m_high.z, p.z) };
				}

				if (m_low.x > m_high.x)
					return;

				m_size[0] = int(std::floor((m_high.x - m_low.x) / cell)) + 1;
				m_size[1] = int(std::floor((m_high.y - m_low.y) / cell)) + 1;
				m_size[2] = int(std::floor((m_high.z - m_low.z) / cell)) + 1;

				std::vector<uint32_t> cells(count, UINT32_MAX);
				m_start.assign(size_t(m_size[0]) * m_size[1] * m_size[2] + 1, 0);
				for (uint32_t i = 0; i < count; ++i)
				{
					if (!used[i])
						continue;

					int x, y, z;
					Cell(Load(positions, i), x, y, z);
					cells[i] = Index(x, y, z);
					++m_start[cells[i] + 1];
				}

				for (size_t c = 1; c < m_start.size(); ++c)
					m_start[c] += m_start[c - 1];

				m_items.resize(m_start.back());
				std::vector<uint32_t> fill(m_start.begin(), m_start.end() - 1);
				for (uint32_t i = 0; i < count; ++i)
				{
					if (cells[i] != UINT32_MAX)
						m_items[fill[cells[i]]++] = i;
				}
			}

			bool Near(Vec p, float margin) const
			{
				return !m_items.empty() && p.x >= m_low.x - margin && p.y >= m_low.y - margin && p.z >= m_low.z - margin && p.x <= m_high.x + margin && p.y <= m_high.y + margin && p.z <= m_high.z + margin;
			}

			void Cell(Vec p, int& x, int& y, int& z) const
			{
				x = int(std::floor((p.x - m_low.x) / m_cell));
				y = int(std::floor((p.y - m_low.y) / m_cell));
				z = int(std::floor((p.z - m_low.z) / m_cell));
			}

			// Visits every vertex in the shell of cubes r steps around the given one
			template<typename Visit>
			void Shell(int x, int y, int z, int r, Visit&& visit) const
			{
				for (int dz = -r; dz <= r; ++dz)
				for (int dy = -r; dy <= r; ++dy)
				{
					const bool inside = r > 0 && std::abs(dz) < r && std::abs(dy) < r;
					for (int dx = -r; dx <= r; dx += inside ? 2 * r : 1)
					{
						const int cx = x + dx, cy = y + dy, cz = z + dz;
						if (cx < 0 || cy < 0 || cz < 0 || cx >= m_size[0] || cy >= m_size[1] || cz >= m_size[2])
							continue;

						const uint32_t c = Index(cx, cy, cz);
						for (uint32_t i = m_start[c]; i < m_start[c + 1]; ++i)
							visit(m_items[i]);
					}
				}
			}

			float CellSize() const { return m_cell; }

		private:
			uint32_t Index(int x, int y, int z) const { return uint32_t((size_t(z) * m_size[1] + y) * m_size[0] + x); }

			float					m_cell;
			Vec						m_low = { FLT_MAX, FLT_MAX, FLT_MAX };
			Vec						m_high = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			int						m_size[3] = {};
			std::vector<uint32_t>	m_start;
			std::vector<uint32_t>	m_items;
		};
	}

	uint64_t Hash(const void* data, size_t size, uint64_t hash)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(data);
		auto mix = [&](uint64_t word)
		{
			hash ^= word + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
			hash *= 0xFF51AFD7ED558CCDull;
			hash ^= hash >> 33;
		};

		size_t i = 0;
		for (; i + 8 <= size; i += 8)
		{
			uint64_t word;
			memcpy(&word, bytes + i, 8);
			mix(word);
		}

		uint64_t tail = size;
		memcpy(&tail, bytes + i, size - i);
		mix(tail ^ (uint64_t(size) << 56));
		return hash;
	}

	int CompareNames(const char* a, const char* b)
	{
		for (;; ++a, ++b)
		{
			const int x = std::tolower(static_cast<unsigned char>(*a)), y = std::tolower(static_cast<unsigned char>(*b));
			if (x != y || !x)
				return x - y;
		}
	}

	uint64_t Fingerprint(const Mesh& mesh)
	{
		uint64_t hash = Hash(&mesh.vertexCount, sizeof(mesh.vertexCount));
		const uint32_t step = (std::max)(1u, mesh.vertexCount / 512);
		for (uint32_t i = 0; i < mesh.vertexCount; i += step)
			hash = Hash(mesh.vertices + size_t(mesh.stride) * i, sizeof(Vec), hash);
		return hash;
	}

	std::vector<float> Positions(const Mesh& mesh)
	{
		std::vector<float> positions(size_t(mesh.vertexCount) * 3);
		for (uint32_t i = 0; i < mesh.vertexCount; ++i)
			memcpy(&positions[i * 3], mesh.vertices + size_t(mesh.stride) * i, sizeof(Vec));
		return positions;
	}

	Binding::Binding(const std::vector<float>& body, const std::vector<uint16_t>& bodyTriangles, const std::vector<float>& armor, const std::vector<uint16_t>& armorTriangles, const Settings& settings)
	{
		const uint32_t bodyCount = uint32_t(body.size() / 3);
		const uint32_t armorCount = uint32_t(armor.size() / 3);
		m_armorCount = armorCount;
		const float collisionDistance = (std::min)(settings.collisionDistance, 7.9f);
		const float conformDistance = settings.conformDistance;
		const float radius = (std::max)(collisionDistance, conformDistance);
		if (radius <= 0.0f || bodyCount > 65536 || armorCount > 65536)
			return;

		std::vector<uint16_t> triangles;
		for (size_t t = 0; t + 2 < bodyTriangles.size(); t += 3)
		{
			const uint16_t* tri = &bodyTriangles[t];
			if (tri[0] < bodyCount && tri[1] < bodyCount && tri[2] < bodyCount && tri[0] != tri[1] && tri[1] != tri[2] && tri[0] != tri[2])
				triangles.insert(triangles.end(), tri, tri + 3);
		}

		// The triangles around each skin vertex
		std::vector<uint32_t> ringStart(bodyCount + 1), ring(triangles.size());
		for (uint16_t index : triangles)
			++ringStart[index + 1];
		for (uint32_t i = 0; i < bodyCount; ++i)
			ringStart[i + 1] += ringStart[i];
		std::vector<uint32_t> fill(ringStart.begin(), ringStart.end() - 1);
		for (uint32_t i = 0; i < triangles.size(); ++i)
			ring[fill[triangles[i]]++] = i / 3;

		std::vector<uint8_t> used(bodyCount);
		for (uint32_t i = 0; i < bodyCount; ++i)
			used[i] = ringStart[i + 1] > ringStart[i];

		const Grid grid(body, used, 1.0f);
		const int rings = int(std::ceil(radius / grid.CellSize()));
		const float radius2 = radius * radius;
		const float collision2 = collisionDistance * collisionDistance;
		const float fullConform = conformDistance * 0.5f;

		struct Neighbor
		{
			float		distance2;
			uint32_t	index;
		};

		for (uint32_t i = 0; i < armorCount; ++i)
		{
			const Vec p = Load(armor, i);
			if (!grid.Near(p, radius))
				continue;

			// The four closest skin vertices, searched outward until no closer one can be left
			Neighbor nearest[4];
			uint32_t found = 0;
			int x, y, z;
			grid.Cell(p, x, y, z);
			for (int r = 0; r <= rings; ++r)
			{
				grid.Shell(x, y, z, r, [&](uint32_t j)
				{
					const Vec d = p - Load(body, j);
					const float d2 = Dot(d, d);
					if (d2 > radius2 || (found == 4 && d2 >= nearest[3].distance2))
						return;

					uint32_t k = found < 4 ? found++ : 3;
					for (; k > 0 && nearest[k - 1].distance2 > d2; --k)
						nearest[k] = nearest[k - 1];
					nearest[k] = { d2, j };
				});

				const float reach = r * grid.CellSize();
				if ((found == 4 && nearest[3].distance2 <= reach * reach) || reach >= radius)
					break;
			}

			if (!found)
				continue;

			if (collisionDistance > 0.0f)
			{
				Collision::Anchor best = {};
				float best2 = collision2;
				float bestRest = 0.0f;
				for (uint32_t n = 0; n < found; ++n)
				{
					for (uint32_t r = ringStart[nearest[n].index]; r < ringStart[nearest[n].index + 1]; ++r)
					{
						const uint16_t* tri = &triangles[ring[r] * 3];
						const Vec a = Load(body, tri[0]), b = Load(body, tri[1]), c = Load(body, tri[2]);

						float u, v;
						ClosestOnTriangle(p, a, b, c, u, v);
						const Vec q = a + (b - a) * u + (c - a) * v;
						const Vec normal = Cross(b - a, c - a);
						const float length = std::sqrt(Dot(normal, normal));
						const float d2 = Dot(p - q, p - q);
						if (length <= 0.0f || d2 > best2)
							continue;

						best2 = d2;
						bestRest = Dot(p - q, normal) / length;
						best = { uint16_t(i), tri[0], tri[1], tri[2], Unorm(u), Unorm(v), int16_t(bestRest * 4096.0f) };
					}
				}

				// A vertex that starts inside the body, like a lining or a strap tucked under, is never pushed
				if (bestRest > 0.0f)
					m_collision.anchors.push_back(best);
			}

			const float closest = std::sqrt(nearest[0].distance2);
			if (closest <= conformDistance)
			{
				const float t = conformDistance > fullConform ? (std::clamp)((closest - fullConform) / (conformDistance - fullConform), 0.0f, 1.0f) : 0.0f;

				Influence influence = { uint16_t(i), Unorm(1.0f - t * t * (3.0f - 2.0f * t)) };
				float weights[4] = {}, total = 0.0f;
				for (uint32_t n = 0; n < found; ++n)
				{
					weights[n] = 1.0f / (nearest[n].distance2 + 0.01f);
					total += weights[n];
				}
				for (uint32_t n = 0; n < found; ++n)
				{
					influence.body[n] = uint16_t(nearest[n].index);
					influence.weight[n] = Unorm(weights[n] / total);
				}

				if (influence.falloff)
					m_influences.push_back(influence);
			}
		}

		m_collision.anchors.shrink_to_fit();
		m_influences.shrink_to_fit();

		if (m_influences.empty())
			return;

		// Connected pieces, joined across UV seams where vertices sit on the same spot
		std::vector<uint32_t> parent(armorCount);
		for (uint32_t i = 0; i < armorCount; ++i)
			parent[i] = i;
		auto root = [&](uint32_t i)
		{
			while (parent[i] != i)
				i = parent[i] = parent[parent[i]];
			return i;
		};
		auto join = [&](uint32_t a, uint32_t b)
		{
			a = root(a);
			b = root(b);
			if (a != b)
				parent[(std::max)(a, b)] = (std::min)(a, b);
		};

		std::map<std::tuple<float, float, float>, uint32_t> spots;
		for (uint32_t i = 0; i < armorCount; ++i)
		{
			auto [it, inserted] = spots.try_emplace({ armor[i * 3], armor[i * 3 + 1], armor[i * 3 + 2] }, i);
			if (!inserted)
				join(i, it->second);
		}

		for (size_t t = 0; t + 2 < armorTriangles.size(); t += 3)
		{
			const uint16_t* tri = &armorTriangles[t];
			if (tri[0] < armorCount && tri[1] < armorCount && tri[2] < armorCount)
			{
				join(tri[0], tri[1]);
				join(tri[1], tri[2]);
			}
		}

		// Larger pieces are cloth or plates, which may bend with the body
		const uint32_t smallPiece = 256;
		std::vector<uint32_t> size(armorCount);
		for (uint32_t i = 0; i < armorCount; ++i)
			++size[root(i)];

		std::vector<uint32_t> piece(armorCount, UINT32_MAX);
		for (uint32_t i = 0; i < armorCount; ++i)
		{
			const uint32_t r = root(i);
			if (size[r] < 2 || size[r] > smallPiece)
				continue;

			if (piece[r] == UINT32_MAX)
			{
				piece[r] = uint32_t(m_pieceStart.size());
				m_pieceStart.push_back(0);
			}
			++m_pieceStart[piece[r]];
		}

		if (m_pieceStart.empty())
			return;

		// Counts to starts, with one more at the end
		uint32_t total = 0;
		for (auto& start : m_pieceStart)
			total += std::exchange(start, total);
		m_pieceStart.push_back(total);

		m_pieceVertices.resize(total);
		std::vector<uint32_t> next(m_pieceStart.begin(), m_pieceStart.end() - 1);
		for (uint32_t i = 0; i < armorCount; ++i)
		{
			const uint32_t r = root(i);
			if (piece[r] != UINT32_MAX)
				m_pieceVertices[next[piece[r]]++] = uint16_t(i);
		}
	}

	uint32_t Collision::Resolve(const Mesh& body, uint8_t* armor, uint32_t armorStride, float clearance) const
	{
		uint32_t moved = 0;
		for (const auto& anchor : anchors)
		{
			// Anchors read from a file are only checked against the armor, the skin is checked here
			if (anchor.a >= body.vertexCount || anchor.b >= body.vertexCount || anchor.c >= body.vertexCount)
				continue;

			const Vec a = Load(body.vertices, body.stride, anchor.a);
			const Vec ab = Load(body.vertices, body.stride, anchor.b) - a;
			const Vec ac = Load(body.vertices, body.stride, anchor.c) - a;
			const Vec normal = Cross(ab, ac);
			const float length = std::sqrt(Dot(normal, normal));
			if (length <= 0.0f)
				continue;

			float* position = reinterpret_cast<float*>(armor + size_t(armorStride) * anchor.vertex);
			const Vec p = { position[0], position[1], position[2] };
			const Vec q = a + ab * (anchor.u / 65535.0f) + ac * (anchor.v / 65535.0f);
			const float depth = (std::min)(anchor.rest / 4096.0f, clearance) - Dot(p - q, normal) / length;
			if (depth <= 1e-4f)
				continue;

			const Vec push = normal * (depth / length);
			position[0] += push.x;
			position[1] += push.y;
			position[2] += push.z;
			++moved;
		}

		return moved;
	}

	static Vec SkinMove(const float* displacement, const uint16_t (&body)[4], const uint16_t (&weight)[4])
	{
		Vec move = {};
		for (uint32_t n = 0; n < 4; ++n)
		{
			if (weight[n])
			{
				const float* d = displacement + body[n] * 3;
				move = move + Vec{ d[0], d[1], d[2] } * (weight[n] / 65535.0f);
			}
		}
		return move;
	}

	void Binding::Conform(const float* displacement, const float* own, float* accumulated) const
	{
		for (const auto& influence : m_influences)
		{
			Vec move = displacement ? SkinMove(displacement, influence.body, influence.weight) : Vec{};
			if (own)
				move = move - Load(own, influence.vertex);

			const float falloff = influence.falloff / 65535.0f;
			float* sum = accumulated + influence.vertex * 4;
			sum[0] += move.x * falloff;
			sum[1] += move.y * falloff;
			sum[2] += move.z * falloff;
			sum[3] += falloff;
		}
	}

	float Binding::Reach(const float* displacement) const
	{
		float reach = 0.0f;
		for (const auto& influence : m_influences)
		{
			const Vec move = SkinMove(displacement, influence.body, influence.weight);
			reach = (std::max)(reach, Dot(move, move) * float(influence.falloff) * float(influence.falloff));
		}
		return std::sqrt(reach) / 65535.0f;
	}

	float Binding::Mismatch(const float* displacement, const float* own) const
	{
		// Only armor that follows the skin fully, further out the TRI is free to differ
		float skin = 0.0f, error = 0.0f;
		uint32_t count = 0;
		for (const auto& influence : m_influences)
		{
			if (influence.falloff != 65535)
				continue;

			const Vec move = SkinMove(displacement, influence.body, influence.weight);
			const Vec stray = move - Load(own, influence.vertex);
			skin += std::sqrt(Dot(move, move));
			error += std::sqrt(Dot(stray, stray));
			++count;
		}

		// Under a hundredth of a unit on average the slider does nothing worth fitting here
		return count && skin > 0.01f * count ? error / skin : -1.0f;
	}

	bool Binding::Stale(const std::string& slider, const std::function<bool()>& judge) const
	{
		{
			std::lock_guard<std::mutex> guard(m_staleLock);
			if (auto it = m_stale.find(slider); it != m_stale.end())
				return it->second;
		}

		const bool stale = judge();
		std::lock_guard<std::mutex> guard(m_staleLock);
		m_stale.emplace(slider, stale);
		return stale;
	}

	uint32_t Binding::ApplyConform(const float* accumulated, uint8_t* armor, uint32_t armorStride) const
	{
		auto moveOf = [&](uint32_t i)
		{
			const float* sum = accumulated + i * 4;
			return sum[3] > 0.0f ? Vec{ sum[0], sum[1], sum[2] } * (1.0f / (std::max)(sum[3], 1.0f)) : Vec{};
		};

		uint32_t moved = 0;
		auto apply = [&](uint32_t i, Vec move)
		{
			if (Dot(move, move) < 1e-10f)
				return;

			float* position = reinterpret_cast<float*>(armor + size_t(armorStride) * i);
			position[0] += move.x;
			position[1] += move.y;
			position[2] += move.z;
			++moved;
		};

		static thread_local std::vector<uint8_t> done;
		done.assign(m_armorCount, 0);

		// A piece moves by the average of its vertices
		for (size_t p = 0; p + 1 < m_pieceStart.size(); ++p)
		{
			const uint32_t first = m_pieceStart[p], last = m_pieceStart[p + 1];
			Vec move = {};
			for (uint32_t k = first; k < last; ++k)
				move = move + moveOf(m_pieceVertices[k]);
			move = move * (1.0f / float(last - first));

			for (uint32_t k = first; k < last; ++k)
			{
				done[m_pieceVertices[k]] = 1;
				apply(m_pieceVertices[k], move);
			}
		}

		// Other skin shapes may have moved vertices this one has no influence on
		for (uint32_t i = 0; i < m_armorCount; ++i)
		{
			if (!done[i])
				apply(i, moveOf(i));
		}

		return moved;
	}

	namespace
	{
		constexpr uint32_t kMorphsMagic = 0x4D464B53;	// SKFM
		constexpr uint32_t kVersion = 2;

		struct Writer
		{
			std::vector<uint8_t> bytes;

			template<typename T> void Put(const T& value)
			{
				const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
				bytes.insert(bytes.end(), p, p + sizeof(T));
			}

			template<typename T> void PutVector(const std::vector<T>& values)
			{
				Put(uint32_t(values.size()));
				const uint8_t* p = reinterpret_cast<const uint8_t*>(values.data());
				bytes.insert(bytes.end(), p, p + values.size() * sizeof(T));
			}
		};

		// Fails rather than reading past the end, a file may be cut short or not one of ours
		struct Reader
		{
			const std::vector<uint8_t>&	bytes;
			size_t						at = 0;
			bool						ok = true;

			template<typename T> T Get()
			{
				T value{};
				if (at + sizeof(T) > bytes.size())
					ok = false;
				else
					memcpy(&value, bytes.data() + at, sizeof(T));
				at += sizeof(T);
				return value;
			}

			template<typename T> std::vector<T> GetVector()
			{
				const uint32_t count = Get<uint32_t>();
				std::vector<T> values;
				if (!ok || at + size_t(count) * sizeof(T) > bytes.size())
				{
					ok = false;
					return values;
				}
				values.resize(count);
				memcpy(values.data(), bytes.data() + at, size_t(count) * sizeof(T));
				at += size_t(count) * sizeof(T);
				return values;
			}
		};

		// The sliders of a TRI in CompareNames order, to find one by name
		std::vector<std::pair<const char*, uint32_t>> SliderIndex(const FitTRI& tri)
		{
			std::vector<std::pair<const char*, uint32_t>> index;
			for (uint32_t i = 0; i < tri.names.size(); ++i)
				index.emplace_back(tri.names[i].c_str(), i);
			std::sort(index.begin(), index.end(), [](const auto& a, const auto& b) { return CompareNames(a.first, b.first) < 0; });
			return index;
		}

		int64_t FindSlider(const std::vector<std::pair<const char*, uint32_t>>& index, const char* name)
		{
			auto it = std::lower_bound(index.begin(), index.end(), name, [](const auto& entry, const char* key) { return CompareNames(entry.first, key) < 0; });
			return it != index.end() && CompareNames(it->first, name) == 0 ? int64_t(it->second) : -1;
		}
	}

	std::vector<uint8_t> ReadFile(const std::filesystem::path& path)
	{
		std::vector<uint8_t> bytes;
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
			return bytes;

		if (fseek(file, 0, SEEK_END) == 0)
		{
			const long size = ftell(file);
			if (size > 0 && fseek(file, 0, SEEK_SET) == 0)
			{
				bytes.resize(size_t(size));
				if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
					bytes.clear();
			}
		}

		fclose(file);
		return bytes;
	}

	bool WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
	{
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);

		std::filesystem::path temporary = path;
		temporary += L".tmp";
		FILE* file = nullptr;
		if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file)
			return false;

		const bool written = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
		if (fclose(file) != 0 || !written)
		{
			std::filesystem::remove(temporary, error);
			return false;
		}

		std::filesystem::rename(temporary, path, error);
		if (error)
			std::filesystem::remove(temporary, error);
		return !error;
	}

	std::string FileName(uint64_t key)
	{
		char name[24];
		snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(key));
		return name;
	}

	void BindingCache::Configure(const Settings& settings, size_t byteLimit)
	{
		std::lock_guard<std::mutex> guard(m_lock);
		m_settings = settings;
		m_byteLimit = byteLimit;
		++m_generation;
		m_entries.clear();
		m_bytes = 0;
	}

	std::shared_ptr<const Binding> BindingCache::Get(const std::vector<float>& body, const std::vector<uint16_t>& bodyTriangles, const std::vector<float>& armor, const std::vector<uint16_t>& armorTriangles)
	{
		std::unique_lock<std::mutex> lock(m_lock);
		const Settings settings = m_settings;
		const uint64_t generation = m_generation;
		lock.unlock();

		uint64_t key = Hash(&settings, sizeof(settings));
		key = Hash(body.data(), body.size() * sizeof(float), key);
		key = Hash(bodyTriangles.data(), bodyTriangles.size() * sizeof(uint16_t), key);
		key = Hash(armor.data(), armor.size() * sizeof(float), key);
		key = Hash(armorTriangles.data(), armorTriangles.size() * sizeof(uint16_t), key);

		std::promise<std::shared_ptr<const Binding>> promise;
		lock.lock();
		auto [it, inserted] = m_entries.try_emplace(key);
		it->second.used = ++m_clock;
		if (!inserted)
		{
			Future building = it->second.binding;
			lock.unlock();
			return building.get();
		}
		it->second.binding = promise.get_future().share();
		lock.unlock();

		std::shared_ptr<const Binding> binding;
		try
		{
			binding = std::make_shared<const Binding>(body, bodyTriangles, armor, armorTriangles, settings);
		}
		catch (...)
		{
		}
		promise.set_value(binding);

		lock.lock();
		it = m_entries.find(key);
		if (generation != m_generation || it == m_entries.end())
			return binding;

		// Dropped rather than kept, the next fit tries again
		if (!binding)
		{
			m_entries.erase(it);
			return binding;
		}

		it->second.bytes = binding->ByteSize();
		m_bytes += it->second.bytes;
		while (m_bytes > m_byteLimit)
		{
			auto oldest = m_entries.end();
			for (auto e = m_entries.begin(); e != m_entries.end(); ++e)
			{
				if (e->second.bytes && e->first != key && (oldest == m_entries.end() || e->second.used < oldest->second.used))
					oldest = e;
			}
			if (oldest == m_entries.end())
				break;

			m_bytes -= oldest->second.bytes;
			m_entries.erase(oldest);
		}

		return binding;
	}

	size_t BindingCache::ByteSize() const
	{
		std::lock_guard<std::mutex> guard(m_lock);
		return m_bytes;
	}

	void BindingCache::Clear()
	{
		std::lock_guard<std::mutex> guard(m_lock);
		++m_generation;
		m_entries.clear();
		m_bytes = 0;
	}

	void FitMorphs::Slider::Apply(uint8_t* armor, uint32_t stride, float factor) const
	{
		const float scale = multiplier * factor;
		for (const auto& delta : deltas)
		{
			float* position = reinterpret_cast<float*>(armor + size_t(stride) * delta.vertex);
			position[0] += delta.x * scale;
			position[1] += delta.y * scale;
			position[2] += delta.z * scale;
		}
	}

	size_t FitMorphs::ByteSize() const
	{
		size_t bytes = sizeof(*this);
		for (const auto& slider : sliders)
			bytes += sizeof(slider) + slider.name.capacity() + slider.deltas.capacity() * sizeof(Delta);
		for (const auto& name : looked)
			bytes += sizeof(name) + name.capacity();
		for (const auto& skin : skins)
			bytes += sizeof(skin) + skin.anchors.capacity() * sizeof(Collision::Anchor);
		return bytes;
	}

	std::vector<uint8_t> FitMorphs::Serialize() const
	{
		Writer w;
		w.Put(kMorphsMagic);
		w.Put(kVersion);
		w.Put(vertexCount);
		w.Put(uint32_t(sliders.size()));
		for (const auto& slider : sliders)
		{
			w.PutVector(std::vector<char>(slider.name.begin(), slider.name.end()));
			w.Put(slider.multiplier);
			w.PutVector(slider.deltas);
		}
		w.Put(uint32_t(looked.size()));
		for (const auto& name : looked)
			w.PutVector(std::vector<char>(name.begin(), name.end()));
		w.Put(uint32_t(skins.size()));
		for (const auto& skin : skins)
			w.PutVector(skin.anchors);
		return std::move(w.bytes);
	}

	std::shared_ptr<const FitMorphs> FitMorphs::Deserialize(const std::vector<uint8_t>& bytes, uint32_t vertexCount)
	{
		Reader r{ bytes };
		if (r.Get<uint32_t>() != kMorphsMagic || r.Get<uint32_t>() != kVersion || r.Get<uint32_t>() != vertexCount)
			return nullptr;

		auto morphs = std::make_shared<FitMorphs>();
		morphs->vertexCount = vertexCount;

		const uint32_t sliderCount = r.Get<uint32_t>();
		for (uint32_t i = 0; r.ok && i < sliderCount; ++i)
		{
			Slider slider;
			const std::vector<char> name = r.GetVector<char>();
			slider.name.assign(name.begin(), name.end());
			slider.multiplier = r.Get<float>();
			slider.deltas = r.GetVector<Delta>();
			morphs->sliders.push_back(std::move(slider));
		}

		const uint32_t lookedCount = r.Get<uint32_t>();
		for (uint32_t i = 0; r.ok && i < lookedCount; ++i)
		{
			const std::vector<char> name = r.GetVector<char>();
			morphs->looked.emplace_back(name.begin(), name.end());
		}

		const uint32_t skinCount = r.Get<uint32_t>();
		for (uint32_t i = 0; r.ok && i < skinCount; ++i)
			morphs->skins.push_back({ r.GetVector<Collision::Anchor>() });

		if (!r.ok || r.at != bytes.size())
			return nullptr;

		for (size_t i = 1; i < morphs->looked.size(); ++i)
		{
			if (CompareNames(morphs->looked[i - 1].c_str(), morphs->looked[i].c_str()) >= 0)
				return nullptr;
		}

		// Every armor index checked, so a damaged file can't reach outside the shape
		for (size_t i = 0; i < morphs->sliders.size(); ++i)
		{
			if (i && CompareNames(morphs->sliders[i - 1].name.c_str(), morphs->sliders[i].name.c_str()) >= 0)
				return nullptr;
			for (const auto& delta : morphs->sliders[i].deltas)
			{
				if (delta.vertex >= vertexCount)
					return nullptr;
			}
		}
		for (const auto& skin : morphs->skins)
		{
			for (const auto& anchor : skin.anchors)
			{
				if (anchor.vertex >= vertexCount)
					return nullptr;
			}
		}

		return morphs;
	}

	FitMorphs FitMorphs::Merge(const FitMorphs& base, FitMorphs added)
	{
		auto byName = [](const auto& a, const auto& b) { return CompareNames(a.c_str(), b.c_str()) < 0; };
		auto sliderByName = [](const Slider& a, const Slider& b) { return CompareNames(a.name.c_str(), b.name.c_str()) < 0; };

		FitMorphs merged;
		merged.vertexCount = added.vertexCount;
		merged.skins = std::move(added.skins);
		std::set_union(added.sliders.begin(), added.sliders.end(), base.sliders.begin(), base.sliders.end(), std::back_inserter(merged.sliders), sliderByName);
		std::set_union(added.looked.begin(), added.looked.end(), base.looked.begin(), base.looked.end(), std::back_inserter(merged.looked), byName);
		return merged;
	}

	FitMorphs Fit(const FitInput& input, BindingCache& bindings, const FitSettings& settings)
	{
		const uint32_t count = uint32_t(input.armorRest.size() / 3);
		FitMorphs morphs;
		morphs.vertexCount = count;

		std::vector<std::pair<std::shared_ptr<const Binding>, const FitSkin*>> bound;
		for (const auto& skin : input.skins)
		{
			auto binding = bindings.Get(skin->rest, skin->triangles, input.armorRest, input.armorTriangles);
			morphs.skins.push_back(binding ? binding->Anchors() : Collision{});
			if (binding && binding->Conforms())
				bound.emplace_back(std::move(binding), skin.get());
		}

		// Looked at whether the skins have them or not, so they aren't asked for again
		morphs.looked = input.wanted;
		std::sort(morphs.looked.begin(), morphs.looked.end(), [](const std::string& a, const std::string& b) { return CompareNames(a.c_str(), b.c_str()) < 0; });
		morphs.looked.erase(std::unique(morphs.looked.begin(), morphs.looked.end(), [](const std::string& a, const std::string& b) { return CompareNames(a.c_str(), b.c_str()) == 0; }), morphs.looked.end());

		if (!settings.conform || bound.empty())
			return morphs;

		const std::vector<std::string>& names = morphs.looked;

		const auto armorIndex = SliderIndex(input.armorTRI);
		std::vector<std::vector<std::pair<const char*, uint32_t>>> skinIndex;
		for (const auto& [binding, skin] : bound)
			skinIndex.push_back(SliderIndex(skin->tri));

		std::vector<float> accumulated(size_t(count) * 4), own, skinDelta, moved(size_t(count) * 3);
		for (const auto& name : names)
		{
			const int64_t armorSlider = FindSlider(armorIndex, name.c_str());
			bool contributes = false, ownReady = false;
			std::fill(accumulated.begin(), accumulated.end(), 0.0f);

			for (size_t k = 0; k < bound.size(); ++k)
			{
				const auto& [binding, skin] = bound[k];
				const uint32_t skinCount = uint32_t(skin->rest.size() / 3);
				const int64_t skinSlider = FindSlider(skinIndex[k], name.c_str());

				// The skin moves armor by a slider its TRI doesn't have, or by one its TRI has out of date with what that
				// moved it by taken back. Otherwise this skin only counts toward the weights.
				bool stale = false;
				if (skinSlider >= 0 && armorSlider >= 0)
				{
					stale = settings.mismatch > 0.0f && binding->Stale(name, [&]()
					{
						std::vector<float> skinFull(size_t(skinCount) * 3, 0.0f), armorFull(size_t(count) * 3, 0.0f);
						skin->tri.delta(uint32_t(skinSlider), skinFull.data());
						input.armorTRI.delta(uint32_t(armorSlider), armorFull.data());
						return binding->Mismatch(skinFull.data(), armorFull.data()) > settings.mismatch;
					});
				}

				if (skinSlider < 0 || (armorSlider >= 0 && !stale))
				{
					binding->Conform(nullptr, nullptr, accumulated.data());
					continue;
				}

				// A slider the armor lacks that barely moves the skin under it, mostly ones for other parts of the body
				skinDelta.assign(size_t(skinCount) * 3, 0.0f);
				skin->tri.delta(uint32_t(skinSlider), skinDelta.data());
				if (!stale && binding->Reach(skinDelta.data()) < settings.smallest)
				{
					binding->Conform(nullptr, nullptr, accumulated.data());
					continue;
				}

				if (stale && !ownReady)
				{
					own.assign(size_t(count) * 3, 0.0f);
					input.armorTRI.delta(uint32_t(armorSlider), own.data());
					ownReady = true;
				}

				binding->Conform(skinDelta.data(), stale ? own.data() : nullptr, accumulated.data());
				contributes = true;
			}

			if (!contributes)
				continue;

			std::fill(moved.begin(), moved.end(), 0.0f);
			bound.front().first->ApplyConform(accumulated.data(), reinterpret_cast<uint8_t*>(moved.data()), sizeof(Vec));

			// Packed the way BodySlide packs TRI deltas, 16 bits per axis scaled by the largest
			float largest = 0.0f;
			for (float value : moved)
				largest = (std::max)(largest, std::fabs(value));
			if (largest < settings.smallest)
				continue;

			FitMorphs::Slider slider;
			slider.name = name;
			slider.multiplier = largest / 32767.0f;
			for (uint32_t i = 0; i < count; ++i)
			{
				const Vec move = Load(moved, i);
				if (Dot(move, move) < settings.smallest * settings.smallest)
					continue;

				const int16_t x = int16_t(std::lround(moved[i * 3] / slider.multiplier));
				const int16_t y = int16_t(std::lround(moved[i * 3 + 1] / slider.multiplier));
				const int16_t z = int16_t(std::lround(moved[i * 3 + 2] / slider.multiplier));
				if (x || y || z)
					slider.deltas.push_back({ uint16_t(i), x, y, z });
			}
			morphs.sliders.push_back(std::move(slider));
		}

		return morphs;
	}
}
