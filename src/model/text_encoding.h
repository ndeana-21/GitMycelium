#pragma once

#include <QString>
#include <QStringList>

#include <string>

namespace gk {

class TextEncoding {
public:
    static QStringList codecs();

    /// The name that best fits `bytes`: a byte order mark if there is one, then
    /// UTF-8 if the bytes decode as UTF-8, then `fallback` (and if that is empty,
    /// the encoding this machine uses).
    static QString detect(const std::string& bytes, const QString& fallback = QString());

    /// Decodes with `codec`; an empty or unknown name means "detect". Never fails:
    /// bytes the codec cannot represent become replacement characters, because
    /// most of a diff is better than none of it.
    static QString decode(const std::string& bytes, const QString& codec);

    static bool isAvailable(const QString& codec);

    static QString systemCodec();
};

} // namespace gk
