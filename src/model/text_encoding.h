#pragma once

#include <QString>
#include <QStringList>

#include <string>

namespace gk {

/// Turns a repository's bytes into text for the viewer.
///
/// A git blob stores bytes and nothing else: no encoding is recorded anywhere, and
/// a file's bytes are whatever the person who wrote it had. Reading them as UTF-8
/// turns a CP1251 source file into question marks — the diff of a Russian file
/// becomes noise, and it looks like the file is broken rather than misread.
class TextEncoding {
public:
    /// The codecs offered in the picker. A curated order first — the ones that
    /// actually turn up in code and prose — then whatever else this build of Qt
    /// knows, so nothing is hidden, only ordered.
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
