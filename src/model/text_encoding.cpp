#include "model/text_encoding.h"

#include <QByteArray>
#include <QLocale>
#include <QStringConverter>
#include <QStringDecoder>

#include <algorithm>
#include <optional>

namespace gk {
namespace {

/// The codecs worth putting at the top of the picker: UTF-8 and UTF-16, the two
/// Russian single-byte encodings, and the Western ones a file from an old project
/// is likely to be in.
const char* const kPreferred[] = {
    "UTF-8", "UTF-16LE", "UTF-16BE", "windows-1251", "KOI8-R", "IBM866", "ISO-8859-5",
    "windows-1252", "ISO-8859-1", "windows-1250", "Shift_JIS", "GB18030",
};

/// A byte order mark, if the bytes start with one. It is the only encoding
/// information the bytes carry, so it wins over any guess.
std::optional<QString> codecFromBom(const std::string& bytes) {
    const QByteArray head = QByteArray::fromRawData(bytes.data(),
                                                    static_cast<qsizetype>(
                                                        std::min<std::size_t>(bytes.size(), 4)));
    if (head.startsWith("\xEF\xBB\xBF")) {
        return QStringLiteral("UTF-8");
    }
    if (head.startsWith("\xFF\xFE")) {
        return QStringLiteral("UTF-16LE");
    }
    if (head.startsWith("\xFE\xFF")) {
        return QStringLiteral("UTF-16BE");
    }
    return std::nullopt;
}

std::optional<QStringConverter::Encoding> builtinFor(const QString& codec) {
    return QStringConverter::encodingForName(codec.toUtf8().constData());
}

QStringDecoder decoderFor(const QString& codec) {
    // Built-in names first, then the platform's own (ICU, on this machine):
    // windows-1251 is not something Qt implements itself.
    if (const auto builtin = builtinFor(codec)) {
        return QStringDecoder(*builtin);
    }
    return QStringDecoder(codec.toUtf8().constData());
}

/// True when the bytes are well-formed UTF-8.
///
/// Written out rather than asked of QStringDecoder: its error flag is not
/// reliable for this question — invalid sequences come back as replacement
/// characters with the flag unset — and "are these bytes UTF-8" decides which
/// codec the whole file is read with. Overlong encodings, surrogates and
/// out-of-range code points are all rejected: none of them occur in real text, and
/// all of them are how CP1251 bytes masquerade as UTF-8.
bool isValidUtf8(const std::string& bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        if (byte < 0x80) {
            ++i;
            continue;
        }

        int extra = 0;
        unsigned int code = 0;
        if ((byte & 0xE0U) == 0xC0U) {
            extra = 1;
            code = byte & 0x1FU;
        } else if ((byte & 0xF0U) == 0xE0U) {
            extra = 2;
            code = byte & 0x0FU;
        } else if ((byte & 0xF8U) == 0xF0U) {
            extra = 3;
            code = byte & 0x07U;
        } else {
            return false; // a continuation byte with nothing to continue
        }

        if (i + static_cast<std::size_t>(extra) >= bytes.size()) {
            return false; // truncated at the end of the file
        }
        for (int k = 1; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(bytes[i + static_cast<std::size_t>(k)]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6) | (next & 0x3FU);
        }

        const unsigned int minimum = extra == 1 ? 0x80U : (extra == 2 ? 0x800U : 0x10000U);
        if (code < minimum || code > 0x10FFFFU || (code >= 0xD800U && code <= 0xDFFFU)) {
            return false;
        }
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

} // namespace

QString TextEncoding::systemCodec() {
    const QString locale = QLocale::system().name();
    if (locale.startsWith(QLatin1String("ru")) || locale.startsWith(QLatin1String("uk")) ||
        locale.startsWith(QLatin1String("be")) || locale.startsWith(QLatin1String("bg"))) {
        // A Russian desktop still has plenty of CP1251 files, and that is what a
        // file with no byte order mark and invalid UTF-8 almost always is.
        if (isAvailable(QStringLiteral("windows-1251"))) {
            return QStringLiteral("windows-1251");
        }
    }
    return QStringLiteral("UTF-8");
}

bool TextEncoding::isAvailable(const QString& codec) {
    if (codec.isEmpty()) {
        return false;
    }
    return decoderFor(codec).isValid();
}

QStringList TextEncoding::codecs() {
    QStringList offered;
    for (const char* name : kPreferred) {
        const QString codec = QString::fromLatin1(name);
        if (isAvailable(codec)) {
            offered.push_back(codec);
        }
    }

    // Whatever else this build knows, so a project in Shift_JIS or CP437 can still
    // be read; simply further down the list.
    QStringList rest = QStringConverter::availableCodecs();
    std::sort(rest.begin(), rest.end(), [](const QString& a, const QString& b) {
        return QString::compare(a, b, Qt::CaseInsensitive) < 0;
    });
    for (const QString& codec : rest) {
        bool already = false;
        for (const QString& known : offered) {
            already = already || QString::compare(known, codec, Qt::CaseInsensitive) == 0;
        }
        if (!already) {
            offered.push_back(codec);
        }
    }
    return offered;
}

QString TextEncoding::detect(const std::string& bytes, const QString& fallback) {
    if (const auto fromBom = codecFromBom(bytes)) {
        return *fromBom;
    }

    // Valid UTF-8 is very strong evidence: random CP1251 text almost never decodes
    // as UTF-8, and real UTF-8 text almost always has a multi-byte character in it.
    if (isValidUtf8(bytes)) {
        return QStringLiteral("UTF-8");
    }

    if (!fallback.isEmpty() && isAvailable(fallback)) {
        return fallback;
    }
    return systemCodec();
}

QString TextEncoding::decode(const std::string& bytes, const QString& codec) {
    if (bytes.empty()) {
        return {};
    }
    const QString chosen = codec.isEmpty() ? detect(bytes) : codec;
    QStringDecoder decoder = decoderFor(chosen);
    if (!decoder.isValid()) {
        decoder = QStringDecoder(QStringConverter::Utf8);
    }
    const QByteArray raw = QByteArray::fromRawData(bytes.data(), static_cast<qsizetype>(bytes.size()));
    return decoder(raw);
}

} // namespace gk
