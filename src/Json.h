#pragma once

#include <memory>
#include <string>
#include <vector>

// The part of jsoncpp's Value that SKEE uses, on top of glaze. Glaze needs C++23 and clashes with the
// Windows.h macros, so it only ever appears in json/Json.cpp.
//
// A Value is a handle: copying one shares the node, assigning to one copies the content in.
// Like jsoncpp, reading a key that doesn't exist adds it as null, and the as* functions convert between
// types where jsoncpp would. Where jsoncpp would throw they return the default instead.
namespace Json
{
	typedef int Int;
	typedef unsigned int UInt;

	enum ValueType
	{
		nullValue,
		intValue,
		realValue,
		stringValue,
		booleanValue,
		arrayValue,
		objectValue
	};

	class Value
	{
	public:
		class Iterator
		{
		public:
			Iterator(std::shared_ptr<void> owner, std::shared_ptr<std::vector<void*>> nodes, size_t index);

			Value& operator*();
			Iterator& operator++();
			bool operator!=(const Iterator& rhs) const { return m_index != rhs.m_index; }

		private:
			std::shared_ptr<std::vector<void*>> m_nodes;
			size_t m_index;
			Value* m_current;
			std::shared_ptr<Value> m_value;
		};

		Value();
		Value(bool value);
		Value(Int value);
		Value(UInt value);
		Value(double value);
		Value(const char* value);
		Value(const std::string& value);
		Value(const Value& rhs) = default;
		Value& operator=(const Value& rhs);

		Value operator[](const char* key);
		Value operator[](const std::string& key);
		Value operator[](int index);
		Value operator[](UInt index);
		void append(const Value& value);

		ValueType type() const;
		bool empty() const;
		UInt size() const;
		bool isMember(const char* key) const;
		bool isArray() const;
		bool isObject() const;
		bool isString() const;
		bool isDouble() const;
		std::vector<std::string> getMemberNames() const;

		bool asBool() const;
		Int asInt() const;
		UInt asUInt() const;
		float asFloat() const;
		double asDouble() const;
		const char* asCString() const;
		std::string asString() const;

		Iterator begin() const;
		Iterator end() const;

	private:
		friend bool Parse(const std::string& text, Value& root);
		friend std::string Write(const Value& root);

		Value(std::shared_ptr<void> owner, void* node);

		std::shared_ptr<void>	m_owner;	// The root of the tree this node lives in
		void*					m_node;
	};

	// Accepts what jsoncpp accepted, including comments and malformed numbers like 3.5.0
	bool Parse(const std::string& text, Value& root);
	std::string Write(const Value& root);
}
