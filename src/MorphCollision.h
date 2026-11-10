#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Fits morphed armor to the morphed body. Armor near the skin is bound to it once, at rest, and from that:
// - Conform gives the armor a morph of its own for every slider the skin's TRI has that the armor's TRI lacks or has out
//   of date, the weighted displacement of the closest skin vertices the way Outfit Studio's conform does. They don't
//   depend on the sliders' values, so they're made once and applied like any TRI morph.
// - Collision pushes armor that still sank under the skin back out along the closest skin triangle, in every update
// Small loose pieces, like chain links or buckles, move as one so they keep their shape.
// Has no game dependencies, so it can be tested on real meshes outside of the game.
namespace MorphCollision
{
	// Positions are full precision floats at the start of each vertex
	struct Mesh
	{
		const uint8_t*	vertices;
		uint32_t		vertexCount;
		uint32_t		stride;
	};

	// Hashes bytes into a running 64 bit hash
	uint64_t Hash(const void* data, size_t size, uint64_t hash = 0x9E3779B97F4A7C15ull);

	// Positions differ with actor weight and every BodySlide build, a few hundred sampled ones tell two meshes apart
	uint64_t Fingerprint(const Mesh& mesh);

	// Slider names compare without case, as TRIs and the mods that set sliders don't agree on it
	int CompareNames(const char* a, const char* b);

	struct Settings
	{
		float	collisionDistance = 2.0f;	// Armor further from the skin than this is never pushed, at most 7.9
		float	conformDistance = 4.0f;		// Armor follows the skin fully up to half of this and fades out at it
	};

	// Packs the positions out of a vertex block, x y z after each other
	std::vector<float> Positions(const Mesh& mesh);

	// Armor vertices bound to the closest skin triangle
	class Collision
	{
	public:
		// The triangle, the weights of b and c and the height above it in 1/4096 units
		struct Anchor
		{
			uint16_t	vertex;
			uint16_t	a, b, c;
			uint16_t	u, v;
			int16_t		rest;
		};

		// Pushes every bound vertex out to its rest distance or the clearance, whichever is smaller. Returns how many moved.
		uint32_t Resolve(const Mesh& body, uint8_t* armor, uint32_t armorStride, float clearance) const;

		std::vector<Anchor>	anchors;
	};

	class Binding
	{
	public:
		// Positions and triangles of the unmorphed skin and armor
		Binding(const std::vector<float>& body, const std::vector<uint16_t>& bodyTriangles, const std::vector<float>& armor, const std::vector<uint16_t>& armorTriangles, const Settings& settings);

		const Collision& Anchors() const { return m_collision; }

		// Adds the move of each armor vertex, and its weight, to four floats per vertex. Displacement is x y z per skin vertex,
		// null to add only the weights. Own is x y z per armor vertex, what the armor's TRI already moves it by, or null.
		void Conform(const float* displacement, const float* own, float* accumulated) const;

		// How far the armor's own move strays from the skin's, over the armor sitting on it, 0 when they agree and
		// 1 or more when the TRI has nothing there. Negative when the skin barely moves under the armor.
		float Mismatch(const float* displacement, const float* own) const;

		// How far the skin moves the armor sitting on it at most, falloff included. Displacement is x y z per skin vertex.
		float Reach(const float* displacement) const;

		// Remembers per slider whether the armor's TRI entry is out of date, judge runs the first time a slider comes up
		bool Stale(const std::string& slider, const std::function<bool()>& judge) const;

		// Moves the armor by what every skin shape's Conform accumulated, averaging where they overlap. Any binding of the
		// armor that Conforms can apply them all, they share its pieces. Returns how many vertices moved.
		uint32_t ApplyConform(const float* accumulated, uint8_t* armor, uint32_t armorStride) const;

		bool Conforms() const { return !m_influences.empty(); }
		size_t ByteSize() const { return sizeof(*this) + m_collision.anchors.capacity() * sizeof(Collision::Anchor) + m_influences.capacity() * sizeof(Influence) + m_pieceStart.capacity() * sizeof(uint32_t) + m_pieceVertices.capacity() * sizeof(uint16_t); }

	private:
		// Closest skin vertices, weights and falloff out of 65535
		struct Influence
		{
			uint16_t	vertex;
			uint16_t	falloff;
			uint16_t	body[4];
			uint16_t	weight[4];
		};

		uint32_t				m_armorCount = 0;
		Collision				m_collision;
		std::vector<Influence>	m_influences;
		std::vector<uint32_t>	m_pieceStart;	// Vertices of each small piece, one run after the other
		std::vector<uint16_t>	m_pieceVertices;

		mutable std::mutex								m_staleLock;
		mutable std::unordered_map<std::string, bool>	m_stale;
	};

	// Bindings by everything they are built from. Built on the caller's thread, and a caller asking for one another
	// thread is building waits for it.
	class BindingCache
	{
	public:
		// Clears what was built with other settings
		void Configure(const Settings& settings, size_t byteLimit);

		// Null if building failed
		std::shared_ptr<const Binding> Get(const std::vector<float>& body, const std::vector<uint16_t>& bodyTriangles, const std::vector<float>& armor, const std::vector<uint16_t>& armorTriangles);

		size_t ByteSize() const;
		void Clear();

	private:
		using Future = std::shared_future<std::shared_ptr<const Binding>>;

		struct Entry
		{
			Future		binding;
			size_t		bytes = 0;
			uint64_t	used = 0;
		};

		Settings	m_settings;
		size_t		m_byteLimit = 128 * 1024 * 1024;
		size_t		m_bytes = 0;
		uint64_t	m_clock = 0;
		uint64_t	m_generation = 0;

		mutable std::mutex					m_lock;
		std::unordered_map<uint64_t, Entry>	m_entries;
	};

	// Files written whole under a temporary name and renamed, so a crash never leaves half of one. Empty when unreadable.
	std::vector<uint8_t> ReadFile(const std::filesystem::path& path);
	bool WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);
	std::string FileName(uint64_t key);

	struct FitSettings
	{
		bool	conform = true;
		float	mismatch = 0.5f;	// How far a TRI slider may stray from the skin before it counts as out of date, 0 never
		float	smallest = 0.01f;	// Moves under this at full strength are left out, a slider moving the armor no further gets nothing
	};

	// The sliders of a TRI by name, delta adds one at full strength onto x y z per vertex
	struct FitTRI
	{
		std::vector<std::string>							names;
		std::function<void(uint32_t slider, float* out)>	delta;
	};

	// A skin shape armor is fitted to
	struct FitSkin
	{
		std::vector<float>		rest;
		std::vector<uint16_t>	triangles;
		FitTRI					tri;
	};

	struct FitInput
	{
		std::vector<float>							armorRest;
		std::vector<uint16_t>						armorTriangles;
		FitTRI										armorTRI;
		std::vector<std::shared_ptr<const FitSkin>>	skins;
		std::vector<std::string>					wanted;	// The sliders to look at, only those actors use
	};

	// What fitting gives a shape of armor for the sliders it looked at, the same for any values of them. Like a TRI that
	// fills in what the armor's own lacks or corrects what it has out of date.
	struct FitMorphs
	{
		struct Delta
		{
			uint16_t	vertex;
			int16_t		x, y, z;
		};

		// Added on top of the armor's own TRI morph of the same name, if it has one
		struct Slider
		{
			std::string			name;
			float				multiplier = 0.0f;
			std::vector<Delta>	deltas;

			void Apply(uint8_t* armor, uint32_t stride, float factor) const;
		};

		uint32_t					vertexCount = 0;
		std::vector<Slider>			sliders;	// In CompareNames order
		std::vector<std::string>	looked;		// Every slider looked at, whether it needed a morph or not, in CompareNames order
		std::vector<Collision>		skins;		// In the order of the input's skins

		// What base had along with what added looked at since
		static FitMorphs Merge(const FitMorphs& base, FitMorphs added);

		size_t ByteSize() const;

		// Null for bytes that aren't morphs for a shape of this size
		std::vector<uint8_t> Serialize() const;
		static std::shared_ptr<const FitMorphs> Deserialize(const std::vector<uint8_t>& bytes, uint32_t vertexCount);
	};

	// Only looks at the wanted sliders, the others can be merged in later
	FitMorphs Fit(const FitInput& input, BindingCache& bindings, const FitSettings& settings);
}
