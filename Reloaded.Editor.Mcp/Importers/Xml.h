#pragma once
// A small XML reader for SVG and Tiled TMX: elements, attributes, text,
// CDATA, comments, processing instructions and a DOCTYPE (skipped), and the
// predefined and numeric entities. No namespaces resolution (prefixes stay in
// the names), no DTD validation, no external entities. Engine-free.
#include "Common.h"
#include <cstring>

namespace Mcp::Import::Xml
{
    struct Node
    {
        std::string name;
        std::vector<std::pair<std::string, std::string>> attributes;
        std::vector<Node> children;
        std::string text; // character data directly inside, concatenated
        int line = 0;
        const std::string* Attribute(const std::string& key) const
        {
            for (const auto& [k, v] : attributes) if (k == key) return &v;
            return nullptr;
        }
        std::string Get(const std::string& key, const std::string& fallback = {}) const
        {
            const auto* v = Attribute(key);
            return v ? *v : fallback;
        }
        bool Has(const std::string& key) const { return Attribute(key) != nullptr; }
        // The name without its namespace prefix (svg:rect -> rect).
        std::string Local() const
        {
            const auto colon = name.find(':');
            return colon == std::string::npos ? name : name.substr(colon + 1);
        }
        // All text inside, children included (a <text> with <tspan>s).
        std::string AllText() const
        {
            std::string out = text;
            for (const auto& c : children) out += c.AllText();
            return out;
        }
    };

    namespace Detail
    {
        inline void Utf8(std::string& out, unsigned long code)
        {
            if (code < 0x80) out += static_cast<char>(code);
            else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3F)); }
            else if (code < 0x10000) { out += static_cast<char>(0xE0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F)); }
            else if (code < 0x110000)
            {
                out += static_cast<char>(0xF0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); out += static_cast<char>(0x80 | (code & 0x3F));
            }
        }
        class Parser
        {
        public:
            explicit Parser(const std::string& source) : s(source) {}
            Node Document()
            {
                if (s.size() > 256u * 1024 * 1024) throw std::runtime_error("The XML file is over 256 MB.");
                if (StartsWith(s, "\xEF\xBB\xBF")) at = 3;
                Misc();
                if (at >= s.size() || s[at] != '<') Fail("The file does not start with an XML element");
                Node root = Element(0);
                Misc();
                if (at < s.size()) Fail("Text after the root element");
                return root;
            }
        private:
            const std::string& s;
            size_t at = 0;
            int line = 1;
            [[noreturn]] void Fail(const std::string& what) const { throw std::runtime_error(what + " (XML line " + std::to_string(line) + ")."); }
            bool Starts(const char* text) const { return s.compare(at, std::strlen(text), text) == 0; }
            void Advance(size_t n) { for (size_t i = 0; i < n && at < s.size(); ++i) if (s[at++] == '\n') ++line; }
            void Space() { while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) Advance(1); }
            void SkipPast(const char* end, const char* what)
            {
                const auto found = s.find(end, at);
                if (found == std::string::npos) Fail(std::string("An unclosed ") + what);
                Advance(found + std::strlen(end) - at);
            }
            // Comments, processing instructions and a DOCTYPE between elements.
            void Misc()
            {
                for (;;)
                {
                    Space();
                    if (Starts("<?")) SkipPast("?>", "processing instruction");
                    else if (Starts("<!--")) SkipPast("-->", "comment");
                    else if (Starts("<!DOCTYPE") || Starts("<!doctype"))
                    {
                        int depth = 0;
                        while (at < s.size())
                        {
                            const char c = s[at];
                            Advance(1);
                            if (c == '[') ++depth;
                            else if (c == ']') --depth;
                            else if (c == '>' && depth <= 0) break;
                        }
                    }
                    else return;
                }
            }
            std::string Name()
            {
                const size_t start = at;
                while (at < s.size() && (std::isalnum(static_cast<unsigned char>(s[at])) || s[at] == '_' || s[at] == ':' || s[at] == '-' || s[at] == '.' || static_cast<unsigned char>(s[at]) >= 0x80)) ++at;
                if (at == start) Fail("A name was expected");
                return s.substr(start, at - start);
            }
            std::string Decode(const std::string& raw) const
            {
                std::string out;
                for (size_t i = 0; i < raw.size(); ++i)
                {
                    if (raw[i] != '&') { out += raw[i]; continue; }
                    const auto end = raw.find(';', i);
                    if (end == std::string::npos || end - i > 12) { out += '&'; continue; }
                    const auto entity = raw.substr(i + 1, end - i - 1);
                    if (entity == "lt") out += '<';
                    else if (entity == "gt") out += '>';
                    else if (entity == "amp") out += '&';
                    else if (entity == "quot") out += '"';
                    else if (entity == "apos") out += '\'';
                    else if (!entity.empty() && entity[0] == '#')
                    {
                        try
                        {
                            const unsigned long code = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X') ? std::stoul(entity.substr(2), nullptr, 16) : std::stoul(entity.substr(1));
                            Utf8(out, code);
                        }
                        catch (const std::exception&) { out += raw.substr(i, end - i + 1); }
                    }
                    else { out += raw.substr(i, end - i + 1); }
                    i = end;
                }
                return out;
            }
            Node Element(int depth)
            {
                if (depth > 256) Fail("Elements nest too deeply");
                Node node;
                node.line = line;
                Advance(1); // <
                node.name = Name();
                for (;;)
                {
                    Space();
                    if (at >= s.size()) Fail("The file ends inside <" + node.name + ">");
                    if (Starts("/>")) { Advance(2); return node; }
                    if (s[at] == '>') { Advance(1); break; }
                    const auto key = Name();
                    Space();
                    if (at >= s.size() || s[at] != '=') Fail("Attribute " + key + " of <" + node.name + "> has no value");
                    Advance(1);
                    Space();
                    if (at >= s.size() || (s[at] != '"' && s[at] != '\'')) Fail("Attribute " + key + " of <" + node.name + "> is not quoted");
                    const char quote = s[at];
                    Advance(1);
                    const auto end = s.find(quote, at);
                    if (end == std::string::npos) Fail("Attribute " + key + " of <" + node.name + "> is not closed");
                    node.attributes.push_back({key, Decode(s.substr(at, end - at))});
                    Advance(end + 1 - at);
                }
                for (;;)
                {
                    if (at >= s.size()) Fail("The file ends before </" + node.name + ">");
                    if (Starts("</"))
                    {
                        Advance(2);
                        const auto closing = Name();
                        if (closing != node.name) Fail("</" + closing + "> closes <" + node.name + ">");
                        Space();
                        if (at >= s.size() || s[at] != '>') Fail("</" + closing + "> is not closed");
                        Advance(1);
                        return node;
                    }
                    if (Starts("<!--")) { SkipPast("-->", "comment"); continue; }
                    if (Starts("<![CDATA["))
                    {
                        Advance(9);
                        const auto end = s.find("]]>", at);
                        if (end == std::string::npos) Fail("An unclosed CDATA section");
                        node.text += s.substr(at, end - at);
                        Advance(end + 3 - at);
                        continue;
                    }
                    if (Starts("<?")) { SkipPast("?>", "processing instruction"); continue; }
                    if (s[at] == '<') { node.children.push_back(Element(depth + 1)); continue; }
                    const auto next = s.find('<', at);
                    const auto end = next == std::string::npos ? s.size() : next;
                    node.text += Decode(s.substr(at, end - at));
                    Advance(end - at);
                }
            }
        };
    }
    // The root element of an XML document.
    inline Node Parse(const std::string& text) { return Detail::Parser(text).Document(); }
}
