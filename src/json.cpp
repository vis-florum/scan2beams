#include "json.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace simple_json {
Value Value::boolean_value(bool v) { Value x;x.type=Type::Boolean;x.boolean=v;return x; }
Value Value::number_value(double v) { Value x;x.type=Type::Number;x.number=v;return x; }
Value Value::string_value(std::string v) { Value x;x.type=Type::String;x.string=std::move(v);return x; }
Value Value::array_value(std::vector<Value> v) { Value x;x.type=Type::Array;x.array=std::move(v);return x; }
Value Value::object_value(std::vector<std::pair<std::string,Value>> v) { Value x;x.type=Type::Object;x.object=std::move(v);return x; }
const Value* Value::find(const std::string& key) const {
    if(type!=Type::Object) return nullptr;
    for(const auto& item:object) if(item.first==key) return &item.second;
    return nullptr;
}
Value* Value::find(const std::string& key) {
    return const_cast<Value*>(static_cast<const Value&>(*this).find(key));
}
const Value& Value::at(const std::string& key) const {
    auto value=find(key);if(!value) throw std::runtime_error("JSON field missing: "+key);return *value;
}
Value& Value::at(const std::string& key) {
    auto value=find(key);if(!value) throw std::runtime_error("JSON field missing: "+key);return *value;
}
void Value::set(const std::string& key,Value value) {
    if(type!=Type::Object) throw std::runtime_error("Cannot add a field to a non-object JSON value");
    if(auto old=find(key)) {*old=std::move(value);return;}
    object.emplace_back(key,std::move(value));
}
namespace {
struct Parser {
    const std::string& text;size_t pos=0;int depth=0;
    [[noreturn]] void fail(const std::string& message) const {
        throw std::runtime_error("Invalid boxes.json at byte "+std::to_string(pos)+": "+message);
    }
    void whitespace() { while(pos<text.size() && (text[pos]==' '||text[pos]=='\t'||text[pos]=='\n'||text[pos]=='\r')) ++pos; }
    bool take(char ch) { whitespace();if(pos<text.size()&&text[pos]==ch){++pos;return true;}return false; }
    static void utf8(std::string& out,uint32_t code) {
        if(code<=0x7f) out.push_back(static_cast<char>(code));
        else if(code<=0x7ff) {out.push_back(static_cast<char>(0xc0|(code>>6)));out.push_back(static_cast<char>(0x80|(code&63)));}
        else if(code<=0xffff) {out.push_back(static_cast<char>(0xe0|(code>>12)));out.push_back(static_cast<char>(0x80|((code>>6)&63)));out.push_back(static_cast<char>(0x80|(code&63)));}
        else {out.push_back(static_cast<char>(0xf0|(code>>18)));out.push_back(static_cast<char>(0x80|((code>>12)&63)));out.push_back(static_cast<char>(0x80|((code>>6)&63)));out.push_back(static_cast<char>(0x80|(code&63)));}
    }
    uint32_t hex4() {
        if(pos+4>text.size()) fail("incomplete Unicode escape");
        uint32_t value=0;
        for(int i=0;i<4;++i) {char ch=text[pos++];value<<=4;
            if(ch>='0'&&ch<='9') value+=ch-'0';else if(ch>='a'&&ch<='f') value+=ch-'a'+10;
            else if(ch>='A'&&ch<='F') value+=ch-'A'+10;else fail("invalid Unicode escape");}
        return value;
    }
    std::string string() {
        whitespace();if(pos>=text.size()||text[pos++]!='"') fail("expected string");std::string out;
        while(pos<text.size()) {unsigned char ch=static_cast<unsigned char>(text[pos++]);
            if(ch=='"') return out;
            if(ch<0x20) fail("control character in string");
            if(ch!='\\') {out.push_back(static_cast<char>(ch));continue;}
            if(pos>=text.size()) fail("incomplete escape");
            char escape=text[pos++];
            switch(escape) {case '"':out.push_back('"');break;case '\\':out.push_back('\\');break;case '/':out.push_back('/');break;
                case 'b':out.push_back('\b');break;case 'f':out.push_back('\f');break;case 'n':out.push_back('\n');break;
                case 'r':out.push_back('\r');break;case 't':out.push_back('\t');break;
                case 'u': {uint32_t code=hex4();
                    if(code>=0xd800&&code<=0xdbff) {if(pos+2>text.size()||text[pos++]!='\\'||text[pos++]!='u') fail("missing low surrogate");uint32_t low=hex4();if(low<0xdc00||low>0xdfff) fail("invalid low surrogate");code=0x10000+((code-0xd800)<<10)+(low-0xdc00);}
                    else if(code>=0xdc00&&code<=0xdfff) fail("unexpected low surrogate");
                    utf8(out,code);break;}
                default:fail("unknown string escape");}
        }
        fail("unterminated string");
    }
    Value value() {
        whitespace();if(++depth>128) fail("nesting is too deep");
        if(pos>=text.size()) fail("expected value");
        Value result;char ch=text[pos];
        if(ch=='"') result=Value::string_value(string());
        else if(ch=='{') {++pos;std::vector<std::pair<std::string,Value>> items;whitespace();
            if(!take('}')) for(;;) {auto key=string();for(const auto& item:items) if(item.first==key) fail("duplicate object field: "+key);
                if(!take(':')) fail("expected ':'");
                items.emplace_back(std::move(key),value());if(take('}')) break;if(!take(',')) fail("expected ',' or '}'");}
            result=Value::object_value(std::move(items));}
        else if(ch=='[') {++pos;std::vector<Value> items;whitespace();
            if(!take(']')) for(;;) {items.push_back(value());if(take(']')) break;if(!take(',')) fail("expected ',' or ']'");}
            result=Value::array_value(std::move(items));}
        else if(text.compare(pos,4,"true")==0) {pos+=4;result=Value::boolean_value(true);}
        else if(text.compare(pos,5,"false")==0) {pos+=5;result=Value::boolean_value(false);}
        else if(text.compare(pos,4,"null")==0) {pos+=4;result=Value{};}
        else {size_t start=pos;if(text[pos]=='-')++pos;if(pos>=text.size())fail("invalid number");
            if(text[pos]=='0')++pos;else {if(text[pos]<'1'||text[pos]>'9')fail("invalid number");while(pos<text.size()&&text[pos]>='0'&&text[pos]<='9')++pos;}
            if(pos<text.size()&&text[pos]=='.') {++pos;size_t digits=pos;while(pos<text.size()&&text[pos]>='0'&&text[pos]<='9')++pos;if(pos==digits)fail("invalid fraction");}
            if(pos<text.size()&&(text[pos]=='e'||text[pos]=='E')) {++pos;if(pos<text.size()&&(text[pos]=='+'||text[pos]=='-'))++pos;size_t digits=pos;while(pos<text.size()&&text[pos]>='0'&&text[pos]<='9')++pos;if(pos==digits)fail("invalid exponent");}
            try {size_t used=0;double number=std::stod(text.substr(start,pos-start),&used);if(used!=pos-start||!std::isfinite(number))fail("non-finite number");result=Value::number_value(number);} catch(const std::exception&) {fail("invalid number");}}
        --depth;return result;
    }
};
void quote(std::ostringstream& out,const std::string& text) {
    out<<'"';for(unsigned char ch:text) {switch(ch) {case '"':out<<"\\\"";break;case '\\':out<<"\\\\";break;
        case '\b':out<<"\\b";break;case '\f':out<<"\\f";break;case '\n':out<<"\\n";break;case '\r':out<<"\\r";break;case '\t':out<<"\\t";break;
        default:if(ch<0x20) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<int>(ch)<<std::dec<<std::setfill(' ');else out<<ch;}}
    out<<'"';
}
void write(std::ostringstream& out,const Value& value,int indent,int level) {
    switch(value.type) {case Value::Type::Null:out<<"null";break;case Value::Type::Boolean:out<<(value.boolean?"true":"false");break;
        case Value::Type::Number:out<<std::setprecision(17)<<value.number;break;case Value::Type::String:quote(out,value.string);break;
        case Value::Type::Array: {out<<'[';if(!value.array.empty()) {bool complex=false;for(const auto& x:value.array) complex|=x.type==Value::Type::Array||x.type==Value::Type::Object;
            for(size_t i=0;i<value.array.size();++i) {if(i)out<<',';if(complex&&indent)out<<'\n'<<std::string((level+1)*indent,' ');else if(i)out<<' ';write(out,value.array[i],indent,level+1);}
            if(complex&&indent)out<<'\n'<<std::string(level*indent,' ');}out<<']';break;}
        case Value::Type::Object: {out<<'{';for(size_t i=0;i<value.object.size();++i) {if(i)out<<',';if(indent)out<<'\n'<<std::string((level+1)*indent,' ');quote(out,value.object[i].first);out<<(indent?": ":":");write(out,value.object[i].second,indent,level+1);}
            if(!value.object.empty()&&indent)out<<'\n'<<std::string(level*indent,' ');
            out<<'}';break;}}
}
}
Value parse_file(const std::filesystem::path& path) {
    auto bytes=std::filesystem::file_size(path);if(bytes>8*1024*1024) throw std::runtime_error("boxes.json exceeds 8 MiB");
    std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("Cannot open "+path.string());
    std::string text((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());Parser parser{text};Value value=parser.value();parser.whitespace();if(parser.pos!=text.size())parser.fail("trailing content");return value;
}
std::string dump(const Value& value,int indent) {std::ostringstream out;write(out,value,std::max(0,indent),0);out<<'\n';return out.str();}
}
