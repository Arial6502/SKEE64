#include "Json.h"

#include <glaze/glaze.hpp>

#include <charconv>
#include <format>
#include <optional>
#include <string_view>

namespace Json
{
	using Node = glz::generic_sorted_i64;

	static Node* AsNode(void* node)
	{
		return static_cast<Node*>(node);
	}

	static const Node* AsNode(const void* node)
	{
		return static_cast<const Node*>(node);
	}

	Value::Iterator::Iterator(std::shared_ptr<void> owner, std::shared_ptr<std::vector<void*>> nodes, size_t index)
		: m_nodes(std::move(nodes))
		, m_index(index)
		, m_current(nullptr)
	{
		m_value = std::make_shared<Value>();
		m_value->m_owner = std::move(owner);
		m_current = m_value.get();
	}

	Value& Value::Iterator::operator*()
	{
		m_current->m_node = (*m_nodes)[m_index];
		return *m_current;
	}

	Value::Iterator& Value::Iterator::operator++()
	{
		++m_index;
		return *this;
	}

	Value::Value()
		: m_owner(std::make_shared<Node>())
		, m_node(m_owner.get())
	{
	}

	Value::Value(std::shared_ptr<void> owner, void* node)
		: m_owner(std::move(owner))
		, m_node(node)
	{
	}

	template<class T>
	static std::shared_ptr<void> MakeRoot(T data)
	{
		auto root = std::make_shared<Node>();
		root->data = std::move(data);
		return root;
	}

	Value::Value(bool value) : m_owner(MakeRoot(value)), m_node(m_owner.get()) {}
	Value::Value(Int value) : m_owner(MakeRoot(int64_t(value))), m_node(m_owner.get()) {}
	Value::Value(UInt value) : m_owner(MakeRoot(int64_t(value))), m_node(m_owner.get()) {}
	Value::Value(double value) : m_owner(MakeRoot(value)), m_node(m_owner.get()) {}
	Value::Value(const char* value) : m_owner(MakeRoot(std::string(value ? value : ""))), m_node(m_owner.get()) {}
	Value::Value(const std::string& value) : m_owner(MakeRoot(value)), m_node(m_owner.get()) {}

	Value& Value::operator=(const Value& rhs)
	{
		if (m_node != rhs.m_node)
			*AsNode(m_node) = *AsNode(rhs.m_node);
		return *this;
	}

	Value Value::operator[](const std::string& key)
	{
		Node* node = AsNode(m_node);
		if (node->is_null())
			node->data = Node::object_t{};
		if (!node->is_object())
			return Value();
		return Value(m_owner, &node->get<Node::object_t>()[key]);
	}

	Value Value::operator[](const char* key)
	{
		return (*this)[std::string(key)];
	}

	Value Value::operator[](int index)
	{
		return (*this)[static_cast<UInt>(index)];
	}

	Value Value::operator[](UInt index)
	{
		Node* node = AsNode(m_node);
		if (!node->is_array() || index >= node->get<Node::array_t>().size())
			return Value();
		return Value(m_owner, &node->get<Node::array_t>()[index]);
	}

	void Value::append(const Value& value)
	{
		Node* node = AsNode(m_node);
		if (node->is_null())
			node->data = Node::array_t{};
		if (node->is_array())
			node->get<Node::array_t>().push_back(*AsNode(value.m_node));
	}

	ValueType Value::type() const
	{
		const Node* node = AsNode(m_node);
		if (node->is_array())
			return arrayValue;
		if (node->is_object())
			return objectValue;
		if (node->is_string())
			return stringValue;
		if (node->is_boolean())
			return booleanValue;
		if (node->holds<int64_t>())
			return intValue;
		if (node->holds<double>())
			return realValue;
		return nullValue;
	}

	UInt Value::size() const
	{
		const Node* node = AsNode(m_node);
		if (node->is_array())
			return static_cast<UInt>(node->get<Node::array_t>().size());
		if (node->is_object())
			return static_cast<UInt>(node->get<Node::object_t>().size());
		return 0;
	}

	bool Value::empty() const
	{
		return AsNode(m_node)->is_null() || ((isArray() || isObject()) && size() == 0);
	}

	bool Value::isMember(const char* key) const
	{
		const Node* node = AsNode(m_node);
		return node->is_object() && node->get<Node::object_t>().contains(key);
	}

	bool Value::isArray() const { return AsNode(m_node)->is_array(); }
	bool Value::isObject() const { return AsNode(m_node)->is_object(); }
	bool Value::isString() const { return AsNode(m_node)->is_string(); }
	bool Value::isDouble() const { return AsNode(m_node)->holds<double>() || AsNode(m_node)->holds<int64_t>(); }

	std::vector<std::string> Value::getMemberNames() const
	{
		std::vector<std::string> names;
		if (isObject())
		{
			for (auto& member : AsNode(m_node)->get<Node::object_t>())
				names.push_back(member.first);
		}
		return names;
	}

	double Value::asDouble() const
	{
		const Node* node = AsNode(m_node);
		if (node->holds<double>())
			return node->get<double>();
		if (node->holds<int64_t>())
			return static_cast<double>(node->get<int64_t>());
		if (node->is_boolean())
			return node->get<bool>() ? 1.0 : 0.0;
		return 0.0;
	}

	bool Value::asBool() const
	{
		const Node* node = AsNode(m_node);
		if (node->is_boolean())
			return node->get<bool>();
		if (node->is_string())
			return !node->get<std::string>().empty();
		return asDouble() != 0.0;
	}

	Int Value::asInt() const
	{
		const Node* node = AsNode(m_node);
		if (node->holds<int64_t>())
			return static_cast<Int>(node->get<int64_t>());
		return static_cast<Int>(asDouble());
	}

	UInt Value::asUInt() const
	{
		const Node* node = AsNode(m_node);
		if (node->holds<int64_t>())
			return static_cast<UInt>(node->get<int64_t>());
		return static_cast<UInt>(static_cast<int64_t>(asDouble()));
	}

	float Value::asFloat() const
	{
		return static_cast<float>(asDouble());
	}

	const char* Value::asCString() const
	{
		const Node* node = AsNode(m_node);
		return node->is_string() ? node->get<std::string>().c_str() : "";
	}

	std::string Value::asString() const
	{
		const Node* node = AsNode(m_node);
		if (node->is_string())
			return node->get<std::string>();
		if (node->is_boolean())
			return node->get<bool>() ? "true" : "false";
		if (node->holds<int64_t>())
			return std::to_string(node->get<int64_t>());
		if (node->holds<double>())
		{
			char buffer[64];
			auto result = std::to_chars(buffer, buffer + sizeof(buffer), node->get<double>());
			return std::string(buffer, result.ptr);
		}
		return {};
	}

	Value::Iterator Value::begin() const
	{
		auto nodes = std::make_shared<std::vector<void*>>();
		Node* node = AsNode(m_node);
		if (node->is_array())
		{
			auto& array = node->get<Node::array_t>();
			nodes->reserve(array.size());
			for (auto& child : array)
				nodes->push_back(&child);
		}
		else if (node->is_object())
		{
			auto& object = node->get<Node::object_t>();
			nodes->reserve(object.size());
			for (auto& child : object)
				nodes->push_back(&child.second);
		}
		return Iterator(m_owner, std::move(nodes), 0);
	}

	Value::Iterator Value::end() const
	{
		return Iterator(m_owner, nullptr, size());
	}

	static bool IsNumberChar(char c)
	{
		return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
	}

	static bool IsDigit(char c)
	{
		return c >= '0' && c <= '9';
	}

	// -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
	static bool IsStrictNumber(std::string_view token)
	{
		size_t i = 0;
		const auto at = [&](size_t index) { return index < token.size() ? token[index] : '\0'; };
		const auto digits = [&]()
		{
			size_t start = i;
			while (IsDigit(at(i)))
				i++;
			return i > start;
		};

		if (at(i) == '-')
			i++;
		if (at(i) == '0')
			i++;
		else if (!digits())
			return false;
		if (at(i) == '.')
		{
			i++;
			if (!digits())
				return false;
		}
		if (at(i) == 'e' || at(i) == 'E')
		{
			i++;
			if (at(i) == '+' || at(i) == '-')
				i++;
			if (!digits())
				return false;
		}
		return i == token.size();
	}

	// jsoncpp read every run of number characters as one token and scanned it with sscanf, so presets holding
	// 3.5.0 loaded as 3.5, and it kept control characters inside strings. Glaze rejects both, so they are
	// rewritten before it sees them. Returns nothing when the text is already valid, which is nearly always.
	static std::optional<std::string> RepairText(const std::string& text)
	{
		std::optional<std::string> repaired;
		size_t copied = 0;
		const auto replace = [&](size_t start, size_t end, std::string_view replacement)
		{
			if (!repaired)
				repaired.emplace();
			repaired->append(text, copied, start - copied);
			repaired->append(replacement);
			copied = end;
		};

		size_t i = 0;
		while (i < text.size())
		{
			char c = text[i];
			if (c == '"')
			{
				for (i++; i < text.size() && text[i] != '"'; i++)
				{
					if (text[i] == '\\')
					{
						i++;
					}
					else if (static_cast<unsigned char>(text[i]) < 0x20)
					{
						char escaped[8];
						auto written = std::format_to_n(escaped, sizeof(escaped), "\\u{:04x}", static_cast<unsigned char>(text[i]));
						replace(i, i + 1, std::string_view(escaped, written.out));
					}
				}
				i++;
			}
			else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
			{
				i = text.find('\n', i);
				if (i == std::string::npos)
					break;
			}
			else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
			{
				i = text.find("*/", i + 2);
				if (i == std::string::npos)
					break;
				i += 2;
			}
			else if (IsDigit(c) || c == '-')
			{
				size_t start = i;
				while (i < text.size() && IsNumberChar(text[i]))
					i++;

				std::string_view token(text.data() + start, i - start);
				if (IsStrictNumber(token))
					continue;

				if (token == "-")
				{
					replace(start, i, "0");
					continue;
				}

				double value = 0.0;
				const char* end = token.data() + token.size();
				auto result = std::from_chars(token.data(), end, value);

				// Nothing usable, or sscanf would have taken the dangling exponent and failed. Glaze rejects these too.
				if (result.ec == std::errc::invalid_argument || (result.ptr != end && (*result.ptr == 'e' || *result.ptr == 'E')))
					continue;

				char buffer[64];
				auto written = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
				replace(start, i, std::string_view(buffer, written.ptr));
			}
			else
			{
				i++;
			}
		}

		if (repaired)
			repaired->append(text, copied, std::string::npos);
		return repaired;
	}

	// Strings are bytes here as they were with jsoncpp, presets can hold text in legacy code pages
	struct ReadOptions : glz::opts
	{
		bool validate_utf8 = false;
	};

	bool Parse(const std::string& text, Value& root)
	{
		static constexpr ReadOptions kReadOptions{ { .comments = true, .error_on_unknown_keys = false } };

		auto document = std::make_shared<Node>();
		const auto repaired = RepairText(text);
		if (glz::read<kReadOptions>(*document, repaired ? *repaired : text))
			return false;

		root.m_node = document.get();
		root.m_owner = std::move(document);
		return true;
	}

	// Keeps sculpt data on one line per vertex, which is most of a preset
	struct WriteOptions : glz::opts
	{
		bool new_lines_in_arrays = false;
	};

	std::string Write(const Value& root)
	{
		static constexpr WriteOptions kWriteOptions{ { .prettify = true } };

		std::string text;
		if (glz::write<kWriteOptions>(*AsNode(root.m_node), text))
			return {};
		return text;
	}
}
