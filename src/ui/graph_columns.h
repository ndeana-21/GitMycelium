#pragma once

#include <string_view>

namespace gk {

/// `Rich` draws the kind glyphs beside each label, `Text` drops them, `Compact`
/// drops them and narrows the label column.
enum class GraphColumnMode {
    Rich,
    Text,
    Compact,
};

inline constexpr GraphColumnMode kDefaultGraphColumnMode = GraphColumnMode::Rich;

/// `QSettings` holds a string so a profile written by a future build with a fourth
/// mode reads as unknown rather than as whichever enumerator shares its number.
inline const char* graphColumnModeKey(GraphColumnMode mode) {
    switch (mode) {
    case GraphColumnMode::Rich: return "rich";
    case GraphColumnMode::Text: return "text";
    case GraphColumnMode::Compact: return "compact";
    }
    return "rich";
}

inline GraphColumnMode graphColumnModeFromKey(const char* key) {
    if (!key) {
        return kDefaultGraphColumnMode;
    }
    const std::string_view text(key);
    if (text == "text") {
        return GraphColumnMode::Text;
    }
    if (text == "compact") {
        return GraphColumnMode::Compact;
    }
    return GraphColumnMode::Rich;
}

} // namespace gk
