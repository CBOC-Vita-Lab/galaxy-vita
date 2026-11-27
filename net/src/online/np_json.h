// Small JSON reader / writer helpers and base64 for the signaling client (server/PROTOCOL.md: JSON over HTTPS, base64 with padding).
// Not a general library: UTF-8 text, numbers as doubles, the nesting depth limited (a hostile server cannot overflow the stack), the text
// limited to 256 KiB and the document to 4000 nodes (a hostile answer cannot take the console's memory), \uXXXX escapes
// below U+10000 turned into UTF-8 (a surrogate becomes '?'). The reader never writes outside its own vectors.
#ifndef NP_NET_JSON_H
#define NP_NET_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace NpNet {

class Json {
public:
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    // A node is an index into the document; -1 = none (every accessor accepts -1 and answers with a default).
    bool Parse(const char* text, size_t len) {
        nodes_.clear();
        root_ = -1;
        if (!text || len > kMaxText) return false;
        pos_ = text;
        end_ = text + len;
        root_ = -1;
        SkipWs();
        root_ = Value(0);
        if (root_ < 0) { nodes_.clear(); return false; }
        SkipWs();
        if (pos_ != end_) { nodes_.clear(); root_ = -1; return false; }
        return true;
    }
    int Root() const { return root_; }
    Type TypeOf(int n) const { return n < 0 ? NUL : nodes_[size_t(n)].type; }
    bool Has(int n) const { return n >= 0 && nodes_[size_t(n)].type != NUL; }
    // member of an object, or -1
    int Get(int n, const char* key) const {
        if (n < 0 || nodes_[size_t(n)].type != OBJ) return -1;
        const Node& o = nodes_[size_t(n)];
        for (size_t i = 0; i < o.kids.size(); ++i)
            if (nodes_[size_t(o.kids[i])].key == key) return o.kids[i];
        return -1;
    }
    int Size(int n) const { return n < 0 ? 0 : int(nodes_[size_t(n)].kids.size()); }
    int At(int n, int i) const { return (n < 0 || i < 0 || i >= Size(n)) ? -1 : nodes_[size_t(n)].kids[size_t(i)]; }
    const std::string& Str(int n) const {
        static const std::string empty;
        return (n >= 0 && nodes_[size_t(n)].type == STR) ? nodes_[size_t(n)].str : empty;
    }
    double Num(int n, double fallback = 0) const { return (n >= 0 && nodes_[size_t(n)].type == NUM) ? nodes_[size_t(n)].num : fallback; }
    bool Bool(int n, bool fallback = false) const { return (n >= 0 && nodes_[size_t(n)].type == BOOL) ? nodes_[size_t(n)].num != 0 : fallback; }
    // shortcuts
    const std::string& GetStr(int n, const char* key) const { return Str(Get(n, key)); }
    double GetNum(int n, const char* key, double fallback = 0) const { return Num(Get(n, key), fallback); }
    bool GetBool(int n, const char* key, bool fallback = false) const { return Bool(Get(n, key), fallback); }

private:
    struct Node {
        Type type;
        double num;
        std::string str, key;
        std::vector<int> kids;
        Node() : type(NUL), num(0) {}
    };
    static const int kMaxDepth = 12;
    static const size_t kMaxNodes = 4000;
    static const size_t kMaxText = 256 * 1024;

    void SkipWs() {
        while (pos_ < end_ && (*pos_ == ' ' || *pos_ == '\t' || *pos_ == '\n' || *pos_ == '\r')) ++pos_;
    }
    int NewNode(Type t) {
        if (nodes_.size() >= kMaxNodes) return -1;
        nodes_.push_back(Node());
        nodes_.back().type = t;
        return int(nodes_.size()) - 1;
    }
    bool ParseString(std::string* out) {
        if (pos_ >= end_ || *pos_ != '"') return false;
        ++pos_;
        out->clear();
        while (pos_ < end_) {
            const unsigned char c = static_cast<unsigned char>(*pos_++);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out->push_back(char(c)); continue; }
            if (pos_ >= end_) return false;
            const char e = *pos_++;
            switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                if (end_ - pos_ < 4) return false;
                unsigned v = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = *pos_++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') v |= unsigned(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= unsigned(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= unsigned(h - 'A' + 10);
                    else return false;
                }
                if (v >= 0xD800 && v < 0xE000) { out->push_back('?'); break; }
                if (v < 0x80) out->push_back(char(v));
                else if (v < 0x800) { out->push_back(char(0xC0 | (v >> 6))); out->push_back(char(0x80 | (v & 0x3F))); }
                else { out->push_back(char(0xE0 | (v >> 12))); out->push_back(char(0x80 | ((v >> 6) & 0x3F))); out->push_back(char(0x80 | (v & 0x3F))); }
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Literal(const char* word) {
        const size_t n = strlen(word);
        if (size_t(end_ - pos_) < n || memcmp(pos_, word, n) != 0) return false;
        pos_ += n;
        return true;
    }
    int Value(int depth) {
        if (depth > kMaxDepth || pos_ >= end_) return -1;
        const char c = *pos_;
        if (c == '{') {
            ++pos_;
            const int self = NewNode(OBJ);
            if (self < 0) return -1;
            SkipWs();
            if (pos_ < end_ && *pos_ == '}') { ++pos_; return self; }
            for (;;) {
                SkipWs();
                std::string key;
                if (!ParseString(&key)) return -1;
                SkipWs();
                if (pos_ >= end_ || *pos_++ != ':') return -1;
                SkipWs();
                const int v = Value(depth + 1);
                if (v < 0) return -1;
                nodes_[size_t(v)].key = key;
                nodes_[size_t(self)].kids.push_back(v);
                SkipWs();
                if (pos_ >= end_) return -1;
                if (*pos_ == ',') { ++pos_; continue; }
                if (*pos_ == '}') { ++pos_; return self; }
                return -1;
            }
        }
        if (c == '[') {
            ++pos_;
            const int self = NewNode(ARR);
            if (self < 0) return -1;
            SkipWs();
            if (pos_ < end_ && *pos_ == ']') { ++pos_; return self; }
            for (;;) {
                SkipWs();
                const int v = Value(depth + 1);
                if (v < 0) return -1;
                nodes_[size_t(self)].kids.push_back(v);
                SkipWs();
                if (pos_ >= end_) return -1;
                if (*pos_ == ',') { ++pos_; continue; }
                if (*pos_ == ']') { ++pos_; return self; }
                return -1;
            }
        }
        if (c == '"') {
            const int self = NewNode(STR);
            if (self < 0) return -1;
            std::string s;
            if (!ParseString(&s)) return -1;
            nodes_[size_t(self)].str.swap(s);
            return self;
        }
        if (c == 't') { if (!Literal("true")) return -1; const int n = NewNode(BOOL); if (n >= 0) nodes_[size_t(n)].num = 1; return n; }
        if (c == 'f') { if (!Literal("false")) return -1; return NewNode(BOOL); }
        if (c == 'n') { if (!Literal("null")) return -1; return NewNode(NUL); }
        if (c == '-' || (c >= '0' && c <= '9')) {
            char buf[40];
            size_t n = 0;
            while (pos_ < end_ && n + 1 < sizeof buf && ((*pos_ >= '0' && *pos_ <= '9') || *pos_ == '-' || *pos_ == '+' || *pos_ == '.' || *pos_ == 'e' || *pos_ == 'E'))
                buf[n++] = *pos_++;
            buf[n] = 0;
            char* endp = 0;
            const double v = strtod(buf, &endp);
            if (endp == buf || *endp) return -1;
            const int self = NewNode(NUM);
            if (self >= 0) nodes_[size_t(self)].num = v;
            return self;
        }
        return -1;
    }

    std::vector<Node> nodes_;
    const char* pos_;
    const char* end_;
    int root_;
};

// ---- writing: the requests are small and flat, built with these -------------------------------------------------------------------
inline void JsonEscapeAppend(std::string* out, const char* s) {
    out->push_back('"');
    for (; s && *s; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') { out->push_back('\\'); out->push_back(char(c)); }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", unsigned(c)); out->append(b); }
        else out->push_back(char(c));
    }
    out->push_back('"');
}

// ---- base64 (standard alphabet, padded) --------------------------------------------------------------------------------------------
inline std::string Base64Encode(const uint8_t* d, size_t n) {
    static const char* A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? uint32_t(d[i + 2]) : 0);
        out.push_back(A[(v >> 18) & 63]);
        out.push_back(A[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? A[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? A[v & 63] : '=');
    }
    return out;
}
// Decodes into `out` (at most `cap` bytes); the byte count, or -1 on a malformed or too long input.
inline int Base64Decode(const std::string& s, uint8_t* out, size_t cap) {
    if (s.size() % 4) return -1;
    size_t n = 0;
    for (size_t i = 0; i < s.size(); i += 4) {
        uint32_t v = 0;
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + size_t(k)];
            int x;
            if (c >= 'A' && c <= 'Z') x = c - 'A';
            else if (c >= 'a' && c <= 'z') x = c - 'a' + 26;
            else if (c >= '0' && c <= '9') x = c - '0' + 52;
            else if (c == '+') x = 62;
            else if (c == '/') x = 63;
            else if (c == '=' && k >= 2 && i + 4 == s.size()) { x = 0; ++pad; }
            else return -1;
            if (pad && c != '=') return -1;
            v = v << 6 | uint32_t(x);
        }
        const int bytes = 3 - pad;
        for (int k = 0; k < bytes; ++k) {
            if (n >= cap) return -1;
            out[n++] = uint8_t(v >> (16 - 8 * k));
        }
    }
    return int(n);
}

}  // namespace NpNet

#endif
