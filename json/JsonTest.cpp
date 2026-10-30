// Pins the jsoncpp behaviour that existing presets and mods rely on.
// Build and run the skee64_json_test target, it exits non-zero on the first failure.
#include "Json.h"

#include <cstdio>
#include <cstdlib>

static void Check(bool condition, const char* what)
{
	if (!condition)
	{
		std::printf("FAILED: %s\n", what);
		std::exit(1);
	}
}

static Json::Value Parse(const std::string& text)
{
	Json::Value root;
	Check(Json::Parse(text, root), text.c_str());
	return root;
}

static bool Rejects(const std::string& text)
{
	Json::Value root;
	return !Json::Parse(text, root);
}

int main()
{
	Check(Parse(R"({"f": 3.5.0})")["f"].asFloat() == 3.5f, "double dotted number keeps its valid prefix");
	Check(Parse(R"({"f": 1.2.3.4, "g": 2})")["g"].asInt() == 2, "values after a repaired number survive");
	Check(Parse(R"({"f": 5.})")["f"].asInt() == 5, "trailing dot");
	Check(Parse(R"({"f": -.5})")["f"].asFloat() == -0.5f, "missing leading zero");
	Check(Parse(R"({"f": 007})")["f"].asInt() == 7, "leading zeros");
	Check(Parse(R"({"f": 1-2})")["f"].asInt() == 1, "inner minus");
	Check(Parse(R"({"f": -})")["f"].asInt() == 0, "lone minus reads as zero");
	Check(Rejects(R"({"f": 1.5e})"), "dangling exponent is an error");
	Check(Rejects(R"({"f": --5})"), "double minus is an error");

	Check(Parse(R"({"s": "3.5.0"})")["s"].asString() == "3.5.0", "number-like strings are left alone");
	Check(Parse("{\"f\": 1 /* 3.5.0 */, \"g\": 2 // 4.5.6\n}")["g"].asInt() == 2, "comments are skipped");
	Check(Parse("{\"s\": \"a\nb\"}")["s"].asString() == "a\nb", "raw control characters in strings");
	Check(Parse("{\"s\": \"\xc7\xd1\xb1\xdb\"}")["s"].asString() == "\xc7\xd1\xb1\xdb", "legacy code page bytes pass through");
	Check(Parse(R"({"a": 1} trailing)")["a"].asInt() == 1, "text after the root value is ignored");

	Json::Value root = Parse(R"({"signature": 1397445445, "negative": -1, "mods": null, "list": [1, 2, 3]})");
	Check(root["signature"].asUInt() == 1397445445u, "large integers stay exact");
	Check(root["negative"].asInt() == -1, "negative integers");
	Check(root["mods"].empty(), "null is empty");
	Check(root["missing"].empty() && root["missing"].asUInt() == 0, "missing keys read as defaults");
	Check(root["list"].size() == 3 && root["list"][1].asInt() == 2, "array access");

	int sum = 0;
	for (auto& item : root["list"])
		sum += item.asInt();
	Check(sum == 6, "array iteration");

	Json::Value object;
	object["name"] = "value";
	object["number"] = 42;
	Json::Value list;
	list.append(object);
	object["number"] = 7;
	Check(list[0]["number"].asInt() == 42, "append copies");

	Json::Value alias = object;
	alias["number"] = 9;
	Check(object["number"].asInt() == 9, "copying a Value shares the node");

	Json::Value reread = Parse(Json::Write(list));
	Check(reread[0]["name"].asString() == "value" && reread[0]["number"].asInt() == 42, "round trip");

	std::printf("all json checks passed\n");
	return 0;
}
