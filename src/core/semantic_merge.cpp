#include "core/semantic_merge.h"

#include <algorithm>
#include <vector>

namespace gk {
namespace {

/// One top-level member, kept as its exact bytes so a merge never reformats a key
/// it did not have to touch. `body` is `"key": value`; the separating comma is not
/// part of it and is put back by position when the document is written out.
struct Member {
    std::string key;   ///< the key's raw text, quotes included
    std::string body;  ///< `"key": value`, exact bytes
    std::string value; ///< the value alone, for comparing sides
};

struct Object {
    bool ok = false;
    bool pretty = false;
    std::string newline = "\n";
    std::string indent;
    std::string closeIndent;
    bool trailingNewline = false;
    std::vector<Member> members;
};

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void skipWhitespace(const std::string& text, std::size_t* pos) {
    while (*pos < text.size() && isSpace(text[*pos])) {
        ++(*pos);
    }
}

bool parseString(const std::string& text, std::size_t* pos, std::string* raw) {
    if (*pos >= text.size() || text[*pos] != '"') {
        return false;
    }
    const std::size_t start = *pos;
    ++(*pos);
    while (*pos < text.size()) {
        const char c = text[*pos];
        if (c == '\\') {
            *pos += 2;
            continue;
        }
        if (c == '"') {
            ++(*pos);
            *raw = text.substr(start, *pos - start);
            return true;
        }
        ++(*pos);
    }
    return false;
}

bool parseValue(const std::string& text, std::size_t* pos) {
    if (*pos >= text.size()) {
        return false;
    }
    const char first = text[*pos];
    if (first == '"') {
        std::string raw;
        return parseString(text, pos, &raw);
    }
    if (first == '{' || first == '[') {
        const char close = first == '{' ? '}' : ']';
        int depth = 0;
        while (*pos < text.size()) {
            const char c = text[*pos];
            if (c == '"') {
                std::string raw;
                if (!parseString(text, pos, &raw)) {
                    return false;
                }
                continue;
            }
            if (c == first) {
                ++depth;
            } else if (c == close) {
                if (--depth == 0) {
                    ++(*pos);
                    return true;
                }
            }
            ++(*pos);
        }
        return false;
    }
    const std::size_t start = *pos;
    while (*pos < text.size() && !isSpace(text[*pos]) && text[*pos] != ',' && text[*pos] != '}' &&
           text[*pos] != ']') {
        ++(*pos);
    }
    return *pos > start;
}

Object parseObject(const std::string& text) {
    Object object;
    object.newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";

    std::size_t pos = 0;
    skipWhitespace(text, &pos);
    if (pos >= text.size() || text[pos] != '{') {
        return object;
    }
    ++pos;

    const std::size_t afterBrace = pos;
    skipWhitespace(text, &pos);
    if (pos >= text.size()) {
        return object;
    }
    // Members on their own lines is what "pretty" means; a lone trailing newline
    // after the closing brace does not make a one-line object multi-line.
    object.pretty = text.substr(afterBrace, pos - afterBrace).find('\n') != std::string::npos;
    if (text[pos] == '}') {
        object.closeIndent = text.substr(afterBrace, pos - afterBrace);
        object.ok = true;
        return object;
    }
    const std::size_t lastNewline = text.rfind('\n', pos);
    object.indent = lastNewline == std::string::npos
                        ? std::string()
                        : text.substr(lastNewline + 1, pos - (lastNewline + 1));

    while (true) {
        const std::size_t keyStart = pos;
        std::string keyRaw;
        if (!parseString(text, &pos, &keyRaw)) {
            return object;
        }
        skipWhitespace(text, &pos);
        if (pos >= text.size() || text[pos] != ':') {
            return object;
        }
        ++pos;
        skipWhitespace(text, &pos);
        const std::size_t valueStart = pos;
        if (!parseValue(text, &pos)) {
            return object;
        }
        Member member;
        member.key = keyRaw;
        member.value = text.substr(valueStart, pos - valueStart);
        member.body = text.substr(keyStart, pos - keyStart);
        object.members.push_back(std::move(member));

        const std::size_t beforeSeparator = pos;
        skipWhitespace(text, &pos);
        if (pos < text.size() && text[pos] == ',') {
            ++pos;
            skipWhitespace(text, &pos);
            if (pos >= text.size()) {
                return object;
            }
            if (text[pos] == '}') {
                object.closeIndent = text.substr(beforeSeparator, pos - beforeSeparator);
                object.ok = true;
                return object; // a trailing comma
            }
            continue;
        }
        if (pos < text.size() && text[pos] == '}') {
            object.closeIndent = text.substr(beforeSeparator, pos - beforeSeparator);
            ++pos;
            break;
        }
        return object;
    }

    const std::size_t rest = pos;
    skipWhitespace(text, &pos);
    if (pos != text.size()) {
        return object; // something follows the object: not a lone document
    }
    object.trailingNewline = text.find('\n', rest) != std::string::npos;
    object.ok = true;
    return object;
}

const Member* findMember(const Object& object, const std::string& key) {
    const auto it = std::find_if(object.members.begin(), object.members.end(),
                                 [&key](const Member& m) { return m.key == key; });
    return it == object.members.end() ? nullptr : &*it;
}

struct Decision {
    const Member* take = nullptr; ///< the member to emit, when decidable
    bool conflict = false;
    const Member* ours = nullptr;
    const Member* base = nullptr;
    const Member* theirs = nullptr;
};

Decision decide(const Member* om, const Member* bm, const Member* tm) {
    Decision d;
    const auto same = [](const Member* a, const Member* b) {
        return a != nullptr && b != nullptr && a->value == b->value;
    };
    if (same(om, tm)) {
        d.take = om;
    } else if (same(om, bm)) {
        d.take = tm; // our side never moved; theirs did (possibly deleting it)
    } else if (same(tm, bm)) {
        d.take = om;
    } else if (bm == nullptr) {
        if (om != nullptr && tm == nullptr) {
            d.take = om;
        } else if (tm != nullptr && om == nullptr) {
            d.take = tm;
        } else if (om != nullptr && tm != nullptr) {
            d.conflict = true;
        }
    } else {
        if (om == nullptr && tm == nullptr) {
            return d;
        }
        d.conflict = true;
    }
    d.ours = om;
    d.base = bm;
    d.theirs = tm;
    return d;
}

/// The side's member line, comma included when another member follows, so a
/// conflict resolved to either side stays valid JSON.
std::string sideLine(const Member* member, const std::string& indent, bool comma) {
    if (member == nullptr) {
        return std::string();
    }
    return indent + member->body + (comma ? "," : "");
}

std::string emitMember(const Decision& d, const std::string& indent, bool comma) {
    if (!d.conflict) {
        return d.take == nullptr ? std::string() : d.take->body + (comma ? "," : "");
    }
    std::string out = "<<<<<<< ours\n";
    if (d.ours != nullptr) {
        out += sideLine(d.ours, indent, comma) + "\n";
    }
    out += "||||||| base\n";
    if (d.base != nullptr) {
        out += sideLine(d.base, indent, comma) + "\n";
    }
    out += "=======\n";
    if (d.theirs != nullptr) {
        out += sideLine(d.theirs, indent, comma) + "\n";
    }
    out += ">>>>>>> theirs";
    return out;
}

} // namespace

SemanticMergeResult semanticMergeJson(const std::string& ours, const std::string& base,
                                      const std::string& theirs) {
    SemanticMergeResult result;
    const Object o = parseObject(ours);
    const Object b = parseObject(base);
    const Object t = parseObject(theirs);
    if (!o.ok || !b.ok || !t.ok) {
        return result;
    }

    // Order: the base's keys first, in the base's order, then keys only a side
    // added, in that side's order.
    std::vector<std::string> order;
    for (const Member& m : b.members) {
        order.push_back(m.key);
    }
    for (const Object* side : {&o, &t}) {
        for (const Member& m : side->members) {
            if (std::find(order.begin(), order.end(), m.key) == order.end()) {
                order.push_back(m.key);
            }
        }
    }

    std::vector<Decision> decisions;
    for (const std::string& key : order) {
        Decision d = decide(findMember(o, key), findMember(b, key), findMember(t, key));
        if (d.take != nullptr || d.conflict) {
            decisions.push_back(d);
            if (d.conflict) {
                ++result.conflicts;
            }
        }
    }

    const std::size_t count = decisions.size();
    std::string merged = "{";
    for (std::size_t i = 0; i < count; ++i) {
        const Decision& d = decisions[i];
        const bool comma = i + 1 < count;
        if (!d.conflict) {
            // A plain member on its own line in a pretty document; in a compact
            // one it follows the previous token directly, unless a conflict block
            // just ended on the same line.
            const bool afterConflict = i > 0 && decisions[i - 1].conflict;
            if (b.pretty || afterConflict) {
                merged += b.newline;
            }
            merged += b.indent;
            merged += emitMember(d, b.indent, comma);
        } else {
            // Markers must begin a line, so a conflict always starts on a fresh one.
            if (!merged.empty() && merged.back() != '\n') {
                merged += b.newline;
            }
            merged += emitMember(d, b.indent, comma);
        }
    }
    // Keep the closing brace off a marker line for the same reason.
    if (count > 0 && decisions.back().conflict &&
        (b.closeIndent.empty() || b.closeIndent[0] != '\n')) {
        merged += b.newline;
    }
    merged += b.closeIndent + "}";
    if (b.trailingNewline) {
        merged += b.newline;
    }

    result.applicable = true;
    result.hasConflicts = result.conflicts > 0;
    result.merged = std::move(merged);
    return result;
}

} // namespace gk
