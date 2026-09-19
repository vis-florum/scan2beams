#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace simple_json {
struct Value {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type=Type::Null;
    bool boolean=false;
    double number=0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string,Value>> object;

    static Value boolean_value(bool value);
    static Value number_value(double value);
    static Value string_value(std::string value);
    static Value array_value(std::vector<Value> value);
    static Value object_value(std::vector<std::pair<std::string,Value>> value);

    const Value& at(const std::string& key) const;
    Value& at(const std::string& key);
    const Value* find(const std::string& key) const;
    Value* find(const std::string& key);
    void set(const std::string& key,Value value);
};
Value parse_file(const std::filesystem::path& path);
std::string dump(const Value& value,int indent=2);
}
