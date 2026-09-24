// ============================================================================
//  Текстовое ядро — реализация.
//
//  Файл сознательно большой: это одна подсистема (текст + Unicode + кодировки +
//  хеши + сравнение + рендер + diff), и она держится вместе, потому что функции
//  переиспользуют друг друга (например, таблицы используют ширину Unicode, а
//  diff — построчный разбор и таблицы). Разнесение по мелким файлам дало бы
//  больше «шапок», чем пользы.
//
//  Разделы:
//    1. ASCII и базовая обработка строк
//    2. Числа: разбор и форматирование
//    3. Unicode: UTF-8/UTF-16, регистр, ширина, категории
//    4. ANSI: цвета и стили
//    5. Экранирование и кодирование (включая base64, URL, CSV, shell, пути)
//    6. Хеши и случайность
//    7. Сравнение, нечёткий поиск, wildcard, natural sort
//    8. Текст, таблицы, разметка, статистика
//    9. Diff: сравнение текстов и патчи
// ============================================================================
#include "core/strings.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <sstream>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace aia::str {

// ---------------------------------------------------------------------------
//  Общие помощники ядра (нужны сразу нескольким разделам этого файла)
// ---------------------------------------------------------------------------
namespace {

/// Позиция «не найдено».
constexpr std::size_t kNpos = std::string_view::npos;

/// Ограничение индекса сверху.
[[nodiscard]] constexpr std::size_t clamp_index(std::size_t value, std::size_t limit) noexcept {
    return value > limit ? limit : value;
}

/// Проверка «codepoint внутри диапазона».
[[nodiscard]] constexpr bool in_range(std::uint32_t cp, std::uint32_t lo,
                                      std::uint32_t hi) noexcept {
    return cp >= lo && cp <= hi;
}

constexpr std::uint32_t kReplacement = 0xFFFDU;
constexpr std::uint32_t kMaxCodepoint = 0x10FFFFU;
constexpr std::uint32_t kSurrogateFirst = 0xD800U;
constexpr std::uint32_t kSurrogateLast = 0xDFFFU;

/// Шестнадцатеричные цифры для hex-вывода.
[[nodiscard]] constexpr std::string_view hex_digits(bool uppercase) noexcept {
    return uppercase ? std::string_view("0123456789ABCDEF") : std::string_view("0123456789abcdef");
}

/// Декодирует один кодпоинт UTF-8, начиная с offset.
/// Возвращает число занятых байт (0 — если offset за пределами строки).
/// При ошибке в *out_cp кладётся U+FFFD, а в строке пропускается один байт —
/// так разбор «мусорного» текста не зацикливается и не падает.
[[nodiscard]] std::size_t utf8_next(std::string_view s, std::size_t offset,
                                    std::uint32_t* out_cp) noexcept {
    const std::size_t size = s.size();
    if (offset >= size) return 0;
    const auto b0 = static_cast<unsigned char>(s[offset]);

    if (b0 < 0x80U) {
        *out_cp = b0;
        return 1;
    }
    if (b0 >= 0xC2U && b0 <= 0xDFU) {
        if (offset + 1 >= size) { *out_cp = kReplacement; return 1; }
        const auto b1 = static_cast<unsigned char>(s[offset + 1]);
        if ((b1 & 0xC0U) != 0x80U) { *out_cp = kReplacement; return 1; }
        *out_cp = ((b0 & 0x1FU) << 6U) | (b1 & 0x3FU);
        return 2;
    }
    if (b0 >= 0xE0U && b0 <= 0xEFU) {
        if (offset + 2 >= size) { *out_cp = kReplacement; return 1; }
        const auto b1 = static_cast<unsigned char>(s[offset + 1]);
        const auto b2 = static_cast<unsigned char>(s[offset + 2]);
        if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U) { *out_cp = kReplacement; return 1; }
        const std::uint32_t cp = ((b0 & 0x0FU) << 12U) | ((b1 & 0x3FU) << 6U) | (b2 & 0x3FU);
        if (cp < 0x800U || in_range(cp, kSurrogateFirst, kSurrogateLast)) {
            *out_cp = kReplacement;
            return 1;
        }
        *out_cp = cp;
        return 3;
    }
    if (b0 >= 0xF0U && b0 <= 0xF4U) {
        if (offset + 3 >= size) { *out_cp = kReplacement; return 1; }
        const auto b1 = static_cast<unsigned char>(s[offset + 1]);
        const auto b2 = static_cast<unsigned char>(s[offset + 2]);
        const auto b3 = static_cast<unsigned char>(s[offset + 3]);
        if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U || (b3 & 0xC0U) != 0x80U) {
            *out_cp = kReplacement;
            return 1;
        }
        const std::uint32_t cp = ((b0 & 0x07U) << 18U) | ((b1 & 0x3FU) << 12U) |
                                 ((b2 & 0x3FU) << 6U) | (b3 & 0x3FU);
        if (cp < 0x10000U || cp > kMaxCodepoint) {
            *out_cp = kReplacement;
            return 1;
        }
        *out_cp = cp;
        return 4;
    }
    *out_cp = kReplacement;
    return 1;
}

}  // namespace


namespace {


/// Признак «пробельный» для trim: учитывает UTF-8 неразрывный пробел (0xC2 0xA0).
[[nodiscard]] bool is_trim_char(char c) noexcept {
    return is_ascii_space(c);
}

/// Разбивает число на группы по три цифры справа: 1234567 → «1 234 567».
[[nodiscard]] std::string group_digits(std::string_view digits, char separator) {
    if (digits.size() <= 3) return std::string(digits);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    const std::size_t head = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
    out.append(digits.substr(0, head));
    for (std::size_t i = head; i < digits.size(); i += 3) {
        out.push_back(separator);
        out.append(digits.substr(i, 3));
    }
    return out;
}

}  // namespace

// ===========================================================================
//  1. ASCII и базовая обработка строк
// ===========================================================================
int ascii_digit_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

std::string upper_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = ascii_upper(c);
    return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

bool istarts_with_ascii(std::string_view s, std::string_view prefix) noexcept {
    if (prefix.size() > s.size()) return false;
    return iequals_ascii(s.substr(0, prefix.size()), prefix);
}

bool iends_with_ascii(std::string_view s, std::string_view suffix) noexcept {
    if (suffix.size() > s.size()) return false;
    return iequals_ascii(s.substr(s.size() - suffix.size()), suffix);
}

bool icontains_ascii(std::string_view s, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (needle.size() > s.size()) return false;
    const std::size_t last = s.size() - needle.size();
    for (std::size_t i = 0; i <= last; ++i) {
        if (iequals_ascii(s.substr(i, needle.size()), needle)) return true;
    }
    return false;
}

int icompare_ascii(std::string_view a, std::string_view b) noexcept {
    const std::size_t limit = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < limit; ++i) {
        const char ca = ascii_lower(a[i]);
        const char cb = ascii_lower(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return prefix.size() <= s.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) noexcept {
    return suffix.size() <= s.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool contains(std::string_view s, std::string_view needle) noexcept {
    return s.find(needle) != kNpos;
}

std::size_t count_occurrences(std::string_view s, std::string_view needle) noexcept {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = s.find(needle, pos)) != kNpos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string_view trim_left(std::string_view s) noexcept {
    std::size_t start = 0;
    while (start < s.size() && is_trim_char(s[start])) ++start;
    return s.substr(start);
}

std::string_view trim_right(std::string_view s) noexcept {
    std::size_t end = s.size();
    while (end > 0 && is_trim_char(s[end - 1])) --end;
    return s.substr(0, end);
}

std::string_view trim(std::string_view s) noexcept {
    return trim_right(trim_left(s));
}

std::string_view trim_chars(std::string_view s, std::string_view set) noexcept {
    std::size_t start = 0;
    while (start < s.size() && set.find(s[start]) != kNpos) ++start;
    std::size_t end = s.size();
    while (end > start && set.find(s[end - 1]) != kNpos) --end;
    return s.substr(start, end - start);
}

std::string_view trim_quotes(std::string_view s) noexcept {
    if (s.size() >= 2) {
        const char first = s.front();
        const char last = s.back();
        if ((first == '"' || first == '\'' || first == '`') && first == last) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

std::string collapse_spaces(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    bool written = false;
    for (char c : s) {
        if (is_trim_char(c)) {
            pending_space = written;
            continue;
        }
        if (pending_space) out.push_back(' ');
        out.push_back(c);
        pending_space = false;
        written = true;
    }
    return out;
}

void trim_inplace(std::string& s) {
    const std::string_view view = trim(s);
    if (view.data() == s.data() && view.size() == s.size()) return;
    std::string tmp(view);
    s.swap(tmp);
}

std::vector<std::string> split(std::string_view s, char sep, bool keep_empty) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(sep, start);
        const std::string_view piece =
            pos == kNpos ? s.substr(start) : s.substr(start, pos - start);
        if (keep_empty || !piece.empty()) out.emplace_back(piece);
        if (pos == kNpos) break;
        start = pos + 1;
    }
    return out;
}

std::vector<std::string> split(std::string_view s, std::string_view sep, bool keep_empty) {
    std::vector<std::string> out;
    if (sep.empty()) {
        out.emplace_back(s);
        return out;
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(sep, start);
        const std::string_view piece =
            pos == kNpos ? s.substr(start) : s.substr(start, pos - start);
        if (keep_empty || !piece.empty()) out.emplace_back(piece);
        if (pos == kNpos) break;
        start = pos + sep.size();
    }
    return out;
}

std::vector<std::string> split_any(std::string_view s, std::string_view separators, bool keep_empty) {
    std::vector<std::string> out;
    std::string current;
    current.reserve(s.size());
    for (char c : s) {
        if (separators.find(c) != kNpos) {
            if (keep_empty || !current.empty()) out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (keep_empty || !current.empty()) out.push_back(std::move(current));
    return out;
}

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_trim_char(s[i])) ++i;
        const std::size_t start = i;
        while (i < s.size() && !is_trim_char(s[i])) ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

std::vector<std::string> split_lines(std::string_view s, bool keep_empty) {
    return split(s, '\n', keep_empty);
}

std::vector<std::string_view> split_views(std::string_view s, char sep, bool keep_empty) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = s.find(sep, start);
        const std::string_view piece =
            pos == kNpos ? s.substr(start) : s.substr(start, pos - start);
        if (keep_empty || !piece.empty()) out.push_back(piece);
        if (pos == kNpos) break;
        start = pos + 1;
    }
    return out;
}

std::vector<std::string_view> line_views(std::string_view s, bool keep_empty) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t pos = s.find('\n', start);
        std::string_view line =
            pos == kNpos ? s.substr(start) : s.substr(start, pos - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (keep_empty || !line.empty()) out.push_back(line);
        if (pos == kNpos) break;
        start = pos + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    std::size_t total = 0;
    for (const auto& part : parts) total += part.size();
    out.reserve(total + sep.size() * (parts.empty() ? 0 : parts.size() - 1));
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

std::string join(const std::vector<std::string_view>& parts, std::string_view sep) {
    std::string out;
    std::size_t total = 0;
    for (auto part : parts) total += part.size();
    out.reserve(total + sep.size() * (parts.empty() ? 0 : parts.size() - 1));
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.append(sep);
        out.append(parts[i]);
    }
    return out;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(s);
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = s.find(from, pos);
        if (found == kNpos) {
            out.append(s.substr(pos));
            break;
        }
        out.append(s.substr(pos, found - pos));
        out.append(to);
        pos = found + from.size();
    }
    return out;
}

std::string replace_first(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(s);
    const std::size_t found = s.find(from);
    if (found == kNpos) return std::string(s);
    std::string out;
    out.reserve(s.size() - from.size() + to.size());
    out.append(s.substr(0, found));
    out.append(to);
    out.append(s.substr(found + from.size()));
    return out;
}

std::string remove_prefix(std::string_view s, std::string_view prefix) {
    return starts_with(s, prefix) ? std::string(s.substr(prefix.size())) : std::string(s);
}

std::string remove_suffix(std::string_view s, std::string_view suffix) {
    return ends_with(s, suffix) ? std::string(s.substr(0, s.size() - suffix.size())) : std::string(s);
}

std::string repeat(std::string_view unit, std::size_t times) {
    std::string out;
    out.reserve(unit.size() * times);
    for (std::size_t i = 0; i < times; ++i) out.append(unit);
    return out;
}

std::string reverse_bytes(std::string_view s) {
    std::string out(s.rbegin(), s.rend());
    return out;
}

std::string pad_right(std::string_view s, int width, char fill) {
    const int current = terminal_width(s);
    if (current >= width) return std::string(s);
    std::string out(s);
    out.append(static_cast<std::size_t>(width - current), fill);
    return out;
}

std::string pad_left(std::string_view s, int width, char fill) {
    const int current = terminal_width(s);
    if (current >= width) return std::string(s);
    std::string out(static_cast<std::size_t>(width - current), fill);
    out.append(s);
    return out;
}

std::string center_text(std::string_view s, int width, char fill) {
    const int current = terminal_width(s);
    if (current >= width) return std::string(s);
    const int total = width - current;
    const int left = total / 2;
    const int right = total - left;
    std::string out(static_cast<std::size_t>(left), fill);
    out.append(s);
    out.append(static_cast<std::size_t>(right), fill);
    return out;
}

std::string indent(std::string_view s, std::string_view prefix) {
    if (prefix.empty()) return std::string(s);
    return std::string(prefix) + std::string(s);
}

std::string indent_lines(std::string_view s, std::string_view prefix, std::string_view skip_prefix) {
    std::string out;
    out.reserve(s.size() + prefix.size() * 8);
    const auto lines = line_views(s, true);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (!line.empty() && !( !skip_prefix.empty() && starts_with(trim_left(line), skip_prefix))) {
            out.append(prefix);
        }
        out.append(line);
        if (i + 1 < lines.size()) out.push_back('\n');
    }
    return out;
}

std::string dedent(std::string_view s) {
    const auto lines = line_views(s, true);
    std::size_t common = std::numeric_limits<std::size_t>::max();
    for (auto line : lines) {
        if (trim(line).empty()) continue;
        std::size_t indent_size = 0;
        while (indent_size < line.size() && (line[indent_size] == ' ' || line[indent_size] == '\t')) {
            ++indent_size;
        }
        common = std::min(common, indent_size);
    }
    if (common == std::numeric_limits<std::size_t>::max() || common == 0) return std::string(s);

    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        out.append(line.size() > common ? line.substr(common) : std::string_view{});
        if (i + 1 < lines.size()) out.push_back('\n');
    }
    return out;
}

std::string normalize_newlines(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\r') {
            out.push_back('\n');
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string ensure_trailing_newline(std::string_view s) {
    std::string out(s);
    if (out.empty() || out.back() != '\n') out.push_back('\n');
    return out;
}

std::string strip_bom(std::string_view s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEFU &&
        static_cast<unsigned char>(s[1]) == 0xBBU && static_cast<unsigned char>(s[2]) == 0xBFU) {
        return std::string(s.substr(3));
    }
    return std::string(s);
}

// ===========================================================================
//  2. Числа: разбор и форматирование
// ===========================================================================
namespace {

/// Убирает разделители разрядов «1_000», «1 000», «1'000» внутри числа.
[[nodiscard]] std::string strip_digit_separators(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '_' || c == '\'' || c == ' ' || c == '\t') continue;
        out.push_back(c);
    }
    return out;
}

/// Определяет основание по префиксу (0x, 0b, 0o) и сдвигает начало.
[[nodiscard]] int detect_base(std::string_view body, std::size_t* prefix_length) noexcept {
    if (body.size() >= 2) {
        const std::string_view head = body.substr(0, 2);
        if (iequals_ascii(head, "0x")) { *prefix_length = 2; return 16; }
        if (iequals_ascii(head, "0b")) { *prefix_length = 2; return 2; }
        if (iequals_ascii(head, "0o")) { *prefix_length = 2; return 8; }
    }
    *prefix_length = 0;
    return 10;
}

/// Точка или запятая как разделитель дробной части.
[[nodiscard]] std::string normalize_decimal_separator(std::string_view s) {
    const bool has_dot = s.find('.') != kNpos;
    const bool has_comma = s.find(',') != kNpos;
    if (has_comma && !has_dot) {
        std::string out(s);
        std::replace(out.begin(), out.end(), ',', '.');
        return out;
    }
    return std::string(s);
}

/// Единица измерения числа байт. Понимает и латиницу (KB, KiB, MB), и кириллицу
/// (КБ, МБ, ГБ) — обе формы одинаково часты в русских интерфейсах.
[[nodiscard]] std::optional<std::uint64_t> parse_size_unit(std::string_view unit) noexcept {
    if (unit.empty()) return 1ULL;                       // просто байты
    const std::string lowered = lower_ascii(unit);
    const bool binary = lowered.find('i') != kNpos;      // KiB / MiB / GiB
    const std::uint64_t base = binary ? 1024ULL : 1000ULL;

    // Кириллические единицы (UTF-8): «кб», «мб», «гб», «тб», «пб».
    static constexpr std::array<std::pair<std::string_view, int>, 10> kCyrillic = {{
        {"кб", 1}, {"мб", 2}, {"гб", 3}, {"тб", 4}, {"пб", 5},
        {"киб", 1}, {"миб", 2}, {"гиб", 3}, {"тиб", 4}, {"пиб", 5},
    }};
    const std::string cyrillic_lowered = lower_unicode(unit);
    for (const auto& [text, power] : kCyrillic) {
        if (starts_with(cyrillic_lowered, text)) {
            std::uint64_t bytes = 1ULL;
            for (int i = 0; i < power; ++i) bytes *= base;
            return bytes;
        }
    }

    switch (lowered[0]) {
        case 'b': return 1ULL;
        case 'k': return base;
        case 'm': return base * base;
        case 'g': return base * base * base;
        case 't': return base * base * base * base;
        case 'p': return base * base * base * base * base;
        default: return std::nullopt;
    }
}

}  // namespace

std::optional<long long> parse_int(std::string_view s, int base) noexcept {
    std::string body = strip_digit_separators(trim(s));
    if (body.empty()) return std::nullopt;

    bool negative = false;
    if (body[0] == '+' || body[0] == '-') {
        negative = body[0] == '-';
        body.erase(0, 1);
    }
    if (body.empty()) return std::nullopt;

    if (base == 0) {
        std::size_t prefix_length = 0;
        base = detect_base(body, &prefix_length);
        body.erase(0, prefix_length);
    } else if ((base == 16 || base == 2 || base == 8) && body.size() >= 2) {
        std::size_t prefix_length = 0;
        const int detected = detect_base(body, &prefix_length);
        if (detected == base) body.erase(0, prefix_length);
    }
    if (base < 2 || base > 36) return std::nullopt;
    if (body.empty()) return std::nullopt;

    // Разбираем вручную: нужно уметь сообщить об отказе без исключений и без локали.
    unsigned long long value = 0;
    const unsigned long long limit =
        negative ? static_cast<unsigned long long>(std::numeric_limits<long long>::max()) + 1ULL
                 : static_cast<unsigned long long>(std::numeric_limits<long long>::max());
    for (char c : body) {
        const int digit = ascii_digit_value(c);
        if (digit < 0 || digit >= base) return std::nullopt;
        const unsigned long long digit_value = static_cast<unsigned long long>(digit);
        if (value > (limit - digit_value) / static_cast<unsigned long long>(base)) return std::nullopt;
        value = value * static_cast<unsigned long long>(base) + digit_value;
    }
    if (negative) {
        if (value == limit) return std::numeric_limits<long long>::min();
        return -static_cast<long long>(value);
    }
    return static_cast<long long>(value);
}

std::optional<unsigned long long> parse_uint(std::string_view s, int base) noexcept {
    std::string body = strip_digit_separators(trim(s));
    if (body.empty()) return std::nullopt;
    if (body[0] == '+') body.erase(0, 1);
    if (body.empty() || body[0] == '-') return std::nullopt;

    if (base == 0) {
        std::size_t prefix_length = 0;
        base = detect_base(body, &prefix_length);
        body.erase(0, prefix_length);
    }
    if (base < 2 || base > 36 || body.empty()) return std::nullopt;

    unsigned long long value = 0;
    const unsigned long long limit = std::numeric_limits<unsigned long long>::max();
    for (char c : body) {
        const int digit = ascii_digit_value(c);
        if (digit < 0 || digit >= base) return std::nullopt;
        const unsigned long long digit_value = static_cast<unsigned long long>(digit);
        if (value > (limit - digit_value) / static_cast<unsigned long long>(base)) return std::nullopt;
        value = value * static_cast<unsigned long long>(base) + digit_value;
    }
    return value;
}

std::optional<double> parse_double(std::string_view s) noexcept {
    std::string body = strip_digit_separators(trim(s));
    if (body.empty()) return std::nullopt;
    body = normalize_decimal_separator(body);

    const bool has_digit = std::any_of(body.begin(), body.end(), [](char c) { return is_ascii_digit(c); });
    if (!has_digit) return std::nullopt;

    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(body.c_str(), &end);
    if (end == nullptr || *end != '\0') return std::nullopt;
    if (errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL)) return std::nullopt;
    return value;
}

std::optional<bool> parse_bool(std::string_view s) noexcept {
    const std::string lowered = lower_unicode(trim(s));   // «Да» и «YES» равнозначны
    if (lowered.empty()) return std::nullopt;
    static const std::array<std::string_view, 10> kTrue = {"true", "yes", "on", "1", "да",
                                                          "истина", "вкл", "y", "t", "включено"};
    static const std::array<std::string_view, 10> kFalse = {"false", "no", "off", "0", "нет",
                                                           "ложь", "выкл", "n", "f", "выключено"};
    for (auto candidate : kTrue) {
        if (lowered == candidate) return true;
    }
    for (auto candidate : kFalse) {
        if (lowered == candidate) return false;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_size(std::string_view s) noexcept {
    const std::string_view cleaned = trim(s);
    if (cleaned.empty()) return std::nullopt;

    // Отделяем числовую часть от единицы измерения.
    std::size_t number_end = 0;
    bool seen_digit = false;
    while (number_end < cleaned.size()) {
        const char c = cleaned[number_end];
        if (is_ascii_digit(c) || c == '.' || c == ',' || c == '_' || c == '\'' ||
            ((c == '+' || c == '-') && number_end == 0)) {
            if (is_ascii_digit(c)) seen_digit = true;
            ++number_end;
            continue;
        }
        break;
    }
    if (!seen_digit) return std::nullopt;

    const auto number = parse_double(cleaned.substr(0, number_end));
    if (!number.has_value() || *number < 0) return std::nullopt;

    std::string_view unit = trim(cleaned.substr(number_end));
    // Единица может быть слитно с числом: «2mb» → number_end уже остановился на 'm'.
    if (!unit.empty() && unit.back() == '.') unit.remove_suffix(1);
    const auto multiplier = parse_size_unit(unit);
    if (!multiplier.has_value()) return std::nullopt;

    const double bytes = *number * static_cast<double>(*multiplier);
    if (bytes > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) return std::nullopt;
    return static_cast<std::uint64_t>(bytes + 0.5);
}

std::optional<double> parse_ratio(std::string_view s) noexcept {
    const std::string_view cleaned = trim(s);
    if (cleaned.empty()) return std::nullopt;

    if (cleaned.back() == '%') {
        const auto value = parse_double(trim(cleaned.substr(0, cleaned.size() - 1)));
        if (!value.has_value()) return std::nullopt;
        return *value / 100.0;
    }
    const std::size_t slash = cleaned.find('/');
    if (slash != kNpos) {
        const auto numerator = parse_double(trim(cleaned.substr(0, slash)));
        const auto denominator = parse_double(trim(cleaned.substr(slash + 1)));
        if (!numerator.has_value() || !denominator.has_value() || *denominator == 0.0) {
            return std::nullopt;
        }
        return *numerator / *denominator;
    }
    return parse_double(cleaned);
}

std::string to_string(long long value) { return std::to_string(value); }
std::string to_string(unsigned long long value) { return std::to_string(value); }
std::string to_string(int value) { return std::to_string(value); }
std::string to_string(unsigned value) { return std::to_string(value); }
std::string to_string(bool value) { return value ? "true" : "false"; }
std::string to_string(char value) { return std::string(1, value); }
std::string to_string(double value) { return format_double(value); }
std::string to_string(float value) { return format_double(static_cast<double>(value)); }

std::string format_double(double value, int precision, bool trim_zeros) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";

    if (precision < 0) precision = 0;
    if (precision > 17) precision = 17;

    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    std::string out = stream.str();

    if (trim_zeros && out.find('.') != std::string::npos) {
        std::size_t end = out.size();
        while (end > 0 && out[end - 1] == '0') --end;
        if (end > 0 && out[end - 1] == '.') --end;
        out.resize(end);
    }
    if (out == "-0") out = "0";
    return out;
}

std::string format_int_grouped(long long value, char separator) {
    const bool negative = value < 0;
    // Работаем со строкой, чтобы корректно обработать LLONG_MIN.
    std::string digits = std::to_string(value);
    if (negative) digits.erase(0, 1);
    std::string grouped = group_digits(digits, separator);
    if (negative) grouped.insert(grouped.begin(), '-');
    return grouped;
}

std::string format_size(std::uint64_t bytes, bool binary, int precision) {
    const double step = binary ? 1024.0 : 1000.0;
    static const std::array<std::string_view, 5> kBinaryUnits = {"Б", "КиБ", "МиБ", "ГиБ", "ТиБ"};
    static const std::array<std::string_view, 5> kDecimalUnits = {"Б", "КБ", "МБ", "ГБ", "ТБ"};

    const auto& units = binary ? kBinaryUnits : kDecimalUnits;
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= step && unit + 1 < units.size()) {
        value /= step;
        ++unit;
    }
    if (unit == 0) return std::to_string(bytes) + " " + std::string(units[0]);
    return format_double(value, precision, true) + " " + std::string(units[unit]);
}

std::string format_duration(double seconds) {
    if (seconds < 0) return "—";
    if (seconds < 1.0) {
        const int millis = static_cast<int>(seconds * 1000.0 + 0.5);
        return std::to_string(millis) + " мс";
    }
    if (seconds < 60.0) return format_double(seconds, 1, true) + " с";
    if (seconds < 3600.0) {
        const int total = static_cast<int>(seconds + 0.5);
        const int minutes = total / 60;
        const int secs = total % 60;
        if (secs == 0) return std::to_string(minutes) + " мин";
        return std::to_string(minutes) + " мин " + std::to_string(secs) + " с";
    }
    if (seconds < 86400.0) {
        const int total = static_cast<int>(seconds + 0.5);
        const int hours = total / 3600;
        const int minutes = (total % 3600) / 60;
        if (minutes == 0) return std::to_string(hours) + " ч";
        return std::to_string(hours) + " ч " + std::to_string(minutes) + " мин";
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int days = total / 86400;
    const int hours = (total % 86400) / 3600;
    if (hours == 0) return std::to_string(days) + " дн";
    return std::to_string(days) + " дн " + std::to_string(hours) + " ч";
}

std::string format_percent(double ratio, int precision) {
    return format_double(ratio * 100.0, precision, true) + "%";
}

std::string format_delta(long long value) {
    if (value > 0) return "+" + std::to_string(value);
    return std::to_string(value);
}

std::string plural_ru(long long n, std::string_view one, std::string_view few, std::string_view many,
                      bool with_number) {
    const long long abs_n = n < 0 ? -n : n;
    const long long last_two = abs_n % 100;
    const long long last = abs_n % 10;

    std::string_view word;
    if (last_two >= 11 && last_two <= 14) word = many;
    else if (last == 1) word = one;
    else if (last >= 2 && last <= 4) word = few;
    else word = many;

    if (!with_number) return std::string(word);
    return std::to_string(n) + " " + std::string(word);
}

std::string plural_en(long long n, std::string_view one, std::string_view many, bool with_number) {
    const std::string_view word = (n == 1) ? one : many;
    if (!with_number) return std::string(word);
    return std::to_string(n) + " " + std::string(word);
}

std::string format_args(std::string_view pattern, const std::vector<std::string>& args) {
    std::string out;
    out.reserve(pattern.size() + args.size() * 8);

    std::size_t auto_index = 0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '{') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '{') {
                out.push_back('{');
                ++i;
                continue;
            }
            const std::size_t close = pattern.find('}', i + 1);
            if (close == kNpos) {
                out.push_back(c);  // незакрытая скобка — оставляем как есть
                continue;
            }
            const std::string_view spec = pattern.substr(i + 1, close - i - 1);

            // Подстановкой считается только «{}» или «{12}». Всё остальное
            // ({:.2f}, {name}, {0!r}) — чужая разметка (шаблоны Python, логи),
            // её надо вернуть как есть, а не съесть аргумент.
            bool is_placeholder = spec.empty();
            std::size_t index = 0;
            if (!spec.empty()) {
                bool digits_only = true;
                for (char spec_char : spec) {
                    if (!is_ascii_digit(spec_char)) {
                        digits_only = false;
                        break;
                    }
                }
                if (digits_only && spec.size() <= 9) {
                    is_placeholder = true;
                    const auto parsed = parse_uint(spec, 10);
                    if (parsed.has_value()) index = static_cast<std::size_t>(*parsed);
                }
            }
            if (!is_placeholder) {
                out.push_back('{');
                out.append(spec);
                out.push_back('}');
                i = close;
                continue;
            }
            if (spec.empty()) index = auto_index++;
            if (index < args.size()) out.append(args[index]);
            else out.append("{").append(spec).append("}");
            i = close;
            continue;
        }
        if (c == '}') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '}') {
                out.push_back('}');
                ++i;
                continue;
            }
            out.push_back(c);
            continue;
        }
        out.push_back(c);
    }
    return out;
}



// ===========================================================================
//  3. Unicode: UTF-8 / UTF-16, регистр, ширина, категории
//
//  Мы не тянем ICU: для задач агента (читать код, считать ширину строки,
//  резать по символам, склонять слова) достаточно алгоритмических правил
//  регистра для латиницы, кириллицы, греческого и армянского плюс таблицы
//  диапазонов для категорий и ширины. Это компактно, быстро и предсказуемо.
// ===========================================================================

namespace {


/// Простая таблица диапазонов вида [lo, hi].
struct Range {
    std::uint32_t lo;
    std::uint32_t hi;
};

[[nodiscard]] bool in_ranges(std::uint32_t cp, const Range* ranges, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (cp >= ranges[i].lo && cp <= ranges[i].hi) return true;
    }
    return false;
}
// --- Таблицы символов нулевой ширины ---------------------------------------
constexpr Range kZeroWidth[] = {
    {0x0000, 0x001F}, {0x007F, 0x009F}, {0x00AD, 0x00AD}, {0x200B, 0x200F},
    {0x2028, 0x202E}, {0x2060, 0x2064}, {0x2066, 0x206F}, {0xFEFF, 0xFEFF},
    {0xFFF9, 0xFFFB}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001}, {0xE0020, 0xE007F},
};

// --- Таблицы комбинирующих символов (диакритика, знаки, вариации) ----------
constexpr Range kCombining[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08E3, 0x0903},
    {0x093A, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0957},
    {0x0962, 0x0963}, {0x0981, 0x0981}, {0x09BC, 0x09BC}, {0x09C1, 0x09C4},
    {0x09CD, 0x09CD}, {0x0A01, 0x0A02}, {0x0A3C, 0x0A3C}, {0x0A41, 0x0A42},
    {0x0A47, 0x0A48}, {0x0A4B, 0x0A4D}, {0x0A70, 0x0A71}, {0x0B01, 0x0B01},
    {0x0B3C, 0x0B3C}, {0x0B3F, 0x0B3F}, {0x0B41, 0x0B44}, {0x0B4D, 0x0B4D},
    {0x0C3E, 0x0C40}, {0x0C46, 0x0C48}, {0x0C4A, 0x0C4D}, {0x0D41, 0x0D44},
    {0x0D4D, 0x0D4D}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EB9}, {0x0EBB, 0x0EBC}, {0x0EC8, 0x0ECD},
    {0x0F18, 0x0F19}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37}, {0x0F39, 0x0F39},
    {0x0F71, 0x0F7E}, {0x0F80, 0x0F84}, {0x0F86, 0x0F87}, {0x0F8D, 0x0F97},
    {0x0F99, 0x0FBC}, {0x102D, 0x1030}, {0x1032, 0x1037}, {0x1039, 0x103A},
    {0x1058, 0x1059}, {0x135D, 0x135F}, {0x1712, 0x1714}, {0x1732, 0x1734},
    {0x1752, 0x1753}, {0x1772, 0x1773}, {0x17B4, 0x17B5}, {0x17B7, 0x17BD},
    {0x17C6, 0x17C6}, {0x17C9, 0x17D3}, {0x17DD, 0x17DD}, {0x180B, 0x180D},
    {0x18A9, 0x18A9}, {0x1920, 0x1922}, {0x1927, 0x1928}, {0x1932, 0x1932},
    {0x1939, 0x193B}, {0x1A17, 0x1A18}, {0x1A1B, 0x1A1B}, {0x1A56, 0x1A56},
    {0x1A58, 0x1A5E}, {0x1A60, 0x1A60}, {0x1A62, 0x1A62}, {0x1A65, 0x1A6C},
    {0x1A73, 0x1A7C}, {0x1A7F, 0x1A7F}, {0x1AB0, 0x1ABE}, {0x1B00, 0x1B03},
    {0x1B34, 0x1B34}, {0x1B36, 0x1B3A}, {0x1B3C, 0x1B3C}, {0x1B42, 0x1B42},
    {0x1B6B, 0x1B73}, {0x1B80, 0x1B81}, {0x1BA2, 0x1BA5}, {0x1BA8, 0x1BA9},
    {0x1BAB, 0x1BAD}, {0x1BE6, 0x1BE6}, {0x1BE8, 0x1BE9}, {0x1BED, 0x1BED},
    {0x1BEF, 0x1BF1}, {0x1C2C, 0x1C33}, {0x1C36, 0x1C37}, {0x1CD0, 0x1CD2},
    {0x1CD4, 0x1CE0}, {0x1CE2, 0x1CE8}, {0x1CED, 0x1CED}, {0x1CF4, 0x1CF4},
    {0x1CF8, 0x1CF9}, {0x1DC0, 0x1DFF}, {0x200C, 0x200D}, {0x20D0, 0x20F0},
    {0x2CEF, 0x2CF1}, {0x2D7F, 0x2D7F}, {0x2DE0, 0x2DFF}, {0x302A, 0x302F},
    {0x3099, 0x309A}, {0xA66F, 0xA672}, {0xA674, 0xA67D}, {0xA69E, 0xA69F},
    {0xA6F0, 0xA6F1}, {0xA802, 0xA802}, {0xA806, 0xA806}, {0xA80B, 0xA80B},
    {0xA825, 0xA826}, {0xA8C4, 0xA8C5}, {0xA8E0, 0xA8F1}, {0xA926, 0xA92D},
    {0xA947, 0xA951}, {0xA980, 0xA982}, {0xA9B3, 0xA9B3}, {0xA9B6, 0xA9B9},
    {0xA9BC, 0xA9BC}, {0xA9E5, 0xA9E5}, {0xAA29, 0xAA2E}, {0xAA31, 0xAA32},
    {0xAA35, 0xAA36}, {0xAA43, 0xAA43}, {0xAA4C, 0xAA4C}, {0xAA7C, 0xAA7C},
    {0xAAB0, 0xAAB0}, {0xAAB2, 0xAAB4}, {0xAAB7, 0xAAB8}, {0xAABE, 0xAABF},
    {0xAAC1, 0xAAC1}, {0xAAEC, 0xAAED}, {0xAAF6, 0xAAF6}, {0xABE5, 0xABE5},
    {0xABE8, 0xABE8}, {0xABED, 0xABED}, {0xFB1E, 0xFB1E}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0x101FD, 0x101FD}, {0x102E0, 0x102E0}, {0x10376, 0x1037A},
    {0x10A01, 0x10A03}, {0x10A05, 0x10A06}, {0x10A0C, 0x10A0F}, {0x10A38, 0x10A3A},
    {0x10A3F, 0x10A3F}, {0x11001, 0x11001}, {0x11038, 0x11046}, {0x1107F, 0x11081},
    {0x11100, 0x11102}, {0x11127, 0x1112B}, {0x1112D, 0x11134}, {0x11173, 0x11173},
    {0x11180, 0x11181}, {0x111B6, 0x111BE}, {0x1122F, 0x11231}, {0x11234, 0x11234},
    {0x11236, 0x11237}, {0x112DF, 0x112DF}, {0x112E3, 0x112EA}, {0x11300, 0x11301},
    {0x1133C, 0x1133C}, {0x11340, 0x11340}, {0x11366, 0x1136C}, {0x11370, 0x11374},
    {0x114B3, 0x114B8}, {0x114BA, 0x114BA}, {0x114BF, 0x114C0}, {0x114C2, 0x114C3},
    {0x115B2, 0x115B5}, {0x115BC, 0x115BD}, {0x115BF, 0x115C0}, {0x115DC, 0x115DD},
    {0x11633, 0x1163A}, {0x1163D, 0x1163D}, {0x1163F, 0x11640}, {0x116AB, 0x116AB},
    {0x116AD, 0x116AD}, {0x116B0, 0x116B5}, {0x116B7, 0x116B7}, {0x1171D, 0x1171F},
    {0x11722, 0x11725}, {0x11727, 0x1172B}, {0x16AF0, 0x16AF4}, {0x16B30, 0x16B36},
    {0x16F8F, 0x16F92}, {0x1BC9D, 0x1BC9E}, {0x1D167, 0x1D169}, {0x1D17B, 0x1D182},
    {0x1D185, 0x1D18B}, {0x1D1AA, 0x1D1AD}, {0x1D242, 0x1D244}, {0x1DA00, 0x1DA36},
    {0x1DA3B, 0x1DA6C}, {0x1DA75, 0x1DA75}, {0x1DA84, 0x1DA84}, {0x1DA9B, 0x1DA9F},
    {0x1DAA1, 0x1DAAF}, {0x1E8D0, 0x1E8D6}, {0xE0100, 0xE01EF},
};

// --- Таблицы «широких» символов (CJK, эмодзи, полноширинные) ----------------
constexpr Range kWide[] = {
    {0x1100, 0x115F}, {0x2329, 0x232A}, {0x2E80, 0x303E}, {0x3041, 0x33FF},
    {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F},
    {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
    {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4}, {0x17000, 0x187F7},
    {0x18800, 0x18CD5}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440},
    {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F},
    {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

// --- Таблицы «широких» интервалов для справки по языкам --------------------
constexpr Range kCjk[] = {
    {0x2E80, 0x2EFF}, {0x3000, 0x303F}, {0x3040, 0x30FF}, {0x3100, 0x312F},
    {0x3130, 0x318F}, {0x3190, 0x319F}, {0x31A0, 0x31BF}, {0x31C0, 0x31EF},
    {0x3200, 0x32FF}, {0x3300, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
    {0xA000, 0xA4CF}, {0xAC00, 0xD7AF}, {0xF900, 0xFAFF}, {0x20000, 0x2A6DF},
    {0x2A700, 0x2EBEF}, {0x30000, 0x3134F},
};

// --- Простые категории ------------------------------------------------------
constexpr Range kPunctuation[] = {
    {0x0021, 0x0023}, {0x0025, 0x002A}, {0x002C, 0x002F}, {0x003A, 0x003B},
    {0x003F, 0x0040}, {0x005B, 0x005D}, {0x005F, 0x005F}, {0x007B, 0x007B},
    {0x007D, 0x007D}, {0x00A1, 0x00A1}, {0x00A7, 0x00A7}, {0x00AB, 0x00AB},
    {0x00B6, 0x00B7}, {0x00BB, 0x00BB}, {0x00BF, 0x00BF}, {0x2010, 0x2027},
    {0x2030, 0x205E}, {0x3001, 0x3003}, {0x3008, 0x3011}, {0x3014, 0x301F},
    {0xFE10, 0xFE19}, {0xFE30, 0xFE52}, {0xFE54, 0xFE61}, {0xFF01, 0xFF03},
    {0xFF05, 0xFF0A}, {0xFF0C, 0xFF0F}, {0xFF1A, 0xFF1B}, {0xFF1F, 0xFF20},
    {0xFF3B, 0xFF3D}, {0xFF5F, 0xFF65},
};

constexpr Range kSymbols[] = {
    {0x0024, 0x0024}, {0x002B, 0x002B}, {0x003C, 0x003E}, {0x005E, 0x005E},
    {0x0060, 0x0060}, {0x007C, 0x007C}, {0x007E, 0x007E}, {0x00A2, 0x00A6},
    {0x00A8, 0x00A9}, {0x00AC, 0x00AC}, {0x00AE, 0x00B1}, {0x00B4, 0x00B4},
    {0x00B8, 0x00B8}, {0x00D7, 0x00D7}, {0x00F7, 0x00F7}, {0x2044, 0x2044},
    {0x20A0, 0x20BF}, {0x2100, 0x214F}, {0x2190, 0x21FF}, {0x2200, 0x22FF},
    {0x2300, 0x23FF}, {0x25A0, 0x25FF}, {0x2600, 0x27BF}, {0x2900, 0x297F},
};

constexpr Range kLettersExtra[] = {
    {0x00AA, 0x00AA}, {0x00B5, 0x00B5}, {0x00BA, 0x00BA}, {0x0370, 0x0374},
    {0x0376, 0x0377}, {0x037A, 0x037D}, {0x037F, 0x037F}, {0x0386, 0x0386},
    {0x0388, 0x038A}, {0x038C, 0x038C}, {0x038E, 0x03A1}, {0x03A3, 0x03F5},
    {0x03F7, 0x0481}, {0x048A, 0x052F}, {0x0531, 0x0556}, {0x0561, 0x0587},
    {0x05D0, 0x05EA}, {0x0620, 0x064A}, {0x066E, 0x066F}, {0x0671, 0x06D3},
    {0x06E5, 0x06E6}, {0x06FA, 0x06FC}, {0x0710, 0x072F}, {0x0904, 0x0939},
    {0x093D, 0x093D}, {0x0950, 0x0950}, {0x0958, 0x0961}, {0x0971, 0x0980},
    {0x0985, 0x098C}, {0x098F, 0x0990}, {0x0993, 0x09A8}, {0x09AA, 0x09B0},
    {0x0A05, 0x0A0A}, {0x0A72, 0x0A74}, {0x0B05, 0x0B0C}, {0x0B85, 0x0B8A},
    {0x0C05, 0x0C0C}, {0x0C85, 0x0C8C}, {0x0D05, 0x0D0C}, {0x0D85, 0x0D96},
    {0x0E01, 0x0E30}, {0x0E32, 0x0E33}, {0x0E40, 0x0E46}, {0x0E81, 0x0E82},
    {0x0F00, 0x0F00}, {0x0F40, 0x0F47}, {0x0F49, 0x0F6C}, {0x1000, 0x102A},
    {0x10A0, 0x10C5}, {0x10D0, 0x10FA}, {0x1100, 0x1248}, {0x13A0, 0x13F5},
    {0x1401, 0x166C}, {0x1681, 0x169A}, {0x16A0, 0x16EA}, {0x1700, 0x170C},
    {0x1E00, 0x1F15}, {0x1F18, 0x1F1D}, {0x1F20, 0x1F45}, {0x1F48, 0x1F4D},
    {0x1F50, 0x1F57}, {0x1F5F, 0x1F7D}, {0x1F80, 0x1FB4}, {0x1FB6, 0x1FBC},
    {0x1FBE, 0x1FBE}, {0x1FC2, 0x1FC4}, {0x1FC6, 0x1FCC}, {0x1FD0, 0x1FD3},
    {0x1FD6, 0x1FDB}, {0x1FE0, 0x1FEC}, {0x1FF2, 0x1FF4}, {0x1FF6, 0x1FFC},
    {0x2C00, 0x2C2E}, {0x2C30, 0x2C5E}, {0x2C60, 0x2CE4}, {0x2D00, 0x2D25},
    {0x2D30, 0x2D67}, {0x3005, 0x3006}, {0x3031, 0x3035}, {0x3041, 0x3096},
    {0x309D, 0x309F}, {0x30A1, 0x30FA}, {0x30FC, 0x30FF}, {0x3105, 0x312F},
    {0x3131, 0x318E}, {0x31A0, 0x31BF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
    {0xA000, 0xA48C}, {0xA4D0, 0xA4FD}, {0xAC00, 0xD7A3}, {0xF900, 0xFA6D},
    {0xFB00, 0xFB06}, {0xFB13, 0xFB17}, {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A},
    {0xFF66, 0xFFBE}, {0x10000, 0x1000B}, {0x1D400, 0x1D7CB}, {0x20000, 0x2A6DF},
};

constexpr Range kEmoji[] = {
    {0x2190, 0x21FF}, {0x2300, 0x23FF}, {0x25A0, 0x27BF}, {0x2B00, 0x2BFF},
    {0x1F000, 0x1F02F}, {0x1F300, 0x1F5FF}, {0x1F600, 0x1F64F}, {0x1F680, 0x1F6FF},
    {0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF},
};

// --- Регистр: парные диапазоны (чётный — заглавная, нечётный — строчная) ---
struct CasePairRange {
    std::uint32_t lo;    // начало, выровнено по чётному «заглавному» коду
    std::uint32_t hi;
    bool lo_is_upper;    // true, если lo — заглавная буква пары
};

constexpr CasePairRange kEvenOddUpper[] = {
    {0x0100, 0x0137, true}, {0x0139, 0x0148, false}, {0x014A, 0x0177, true},
    {0x0179, 0x017E, false}, {0x01DE, 0x01EF, true}, {0x01F8, 0x021F, true},
    {0x0222, 0x0233, true}, {0x0246, 0x024F, true}, {0x0372, 0x0373, true},
    {0x03D8, 0x03EF, true}, {0x0460, 0x0481, true}, {0x048A, 0x04BF, true},
    {0x04D0, 0x052F, true}, {0x1E00, 0x1E95, true}, {0x1EA0, 0x1EFF, true},
    {0x2C67, 0x2C6C, true}, {0x2C8D, 0x2C8F, true}, {0xA640, 0xA66D, true},
    {0xA680, 0xA69B, true}, {0xA722, 0xA72F, true}, {0xA732, 0xA76F, true},
    {0xA779, 0xA77C, false}, {0xFF21, 0xFF3A, true},
};

constexpr Range kCyrillic[] = {
    {0x0400, 0x0484}, {0x0487, 0x052F}, {0x1C80, 0x1C88}, {0x1D2B, 0x1D2B},
    {0x1D78, 0x1D78}, {0x2DE0, 0x2DFF}, {0xA640, 0xA69F}, {0xFE2E, 0xFE2F},
};

constexpr Range kLatin[] = {
    {0x0041, 0x005A}, {0x0061, 0x007A}, {0x00AA, 0x00AA}, {0x00BA, 0x00BA},
    {0x00C0, 0x00D6}, {0x00D8, 0x00F6}, {0x00F8, 0x02B8}, {0x1E00, 0x1EFF},
    {0x2071, 0x2071}, {0x207F, 0x207F}, {0x2090, 0x209C}, {0x212A, 0x212B},
    {0x2132, 0x2132}, {0x214E, 0x214E}, {0x2C60, 0x2C7F}, {0xA722, 0xA787},
    {0xAB30, 0xAB5A}, {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A},
};

constexpr Range kGreek[] = {
    {0x0370, 0x0373}, {0x0375, 0x0377}, {0x037A, 0x037D}, {0x037F, 0x037F},
    {0x0384, 0x0384}, {0x0386, 0x0386}, {0x0388, 0x038A}, {0x038C, 0x038C},
    {0x038E, 0x03A1}, {0x03A3, 0x03E1}, {0x03F0, 0x03FF}, {0x1D26, 0x1D2A},
    {0x1D5D, 0x1D61}, {0x1D66, 0x1D6A}, {0x1DBF, 0x1DBF}, {0x1F00, 0x1F15},
    {0x1F18, 0x1F1D}, {0x1F20, 0x1F45}, {0x1F48, 0x1F4D}, {0x1F50, 0x1F57},
    {0x1F5F, 0x1F7D}, {0x1F80, 0x1FB4}, {0x1FB6, 0x1FC4}, {0x1FC6, 0x1FD3},
    {0x1FD6, 0x1FDB}, {0x1FDD, 0x1FEF}, {0x1FF2, 0x1FF4}, {0x1FF6, 0x1FFE},
};

/// Основная функция смены регистра: алгоритмические правила вместо таблиц ICU.
/// Длина первого кодпоинта в байтах (для capitalize/title_case).
[[nodiscard]] std::size_t first_codepoint_length(std::string_view s) noexcept {
    if (s.empty()) return 0;
    std::uint32_t cp = 0;
    const std::size_t consumed = utf8_next(s, 0, &cp);
    return consumed == 0 ? 0 : consumed;
}

[[nodiscard]] std::uint32_t case_shift(std::uint32_t cp, bool to_upper) noexcept {
    if (cp < 0x80U) {
        return to_upper ? static_cast<std::uint32_t>(ascii_upper(static_cast<char>(cp)))
                        : static_cast<std::uint32_t>(ascii_lower(static_cast<char>(cp)));
    }

    // Латинский-1: пары C0..DE / E0..FE (кроме × и ÷).
    if (to_upper) {
        if (in_range(cp, 0x00E0, 0x00FE) && cp != 0x00F7) return cp - 0x20U;
        if (cp == 0x00FF) return 0x0178U;
        if (cp == 0x00B5) return 0x039CU;               // µ → Μ
        if (cp == 0x00DF) return 0x00DFU;               // ß не имеет одиночной заглавной
        if (cp == 0x0131) return 0x0049U;               // ı → I
        if (cp == 0x0130) return 0x0130U;
        if (in_range(cp, 0x03C2, 0x03C2)) return 0x03A3U;  // ς → Σ
        if (in_range(cp, 0x03B1, 0x03C1)) return cp - 0x20U;
        if (in_range(cp, 0x03C3, 0x03CB)) return cp - 0x20U;
        if (in_range(cp, 0x0430, 0x044F)) return cp - 0x20U;  // а-я → А-Я
        if (in_range(cp, 0x0450, 0x045F)) return cp - 0x50U;  // ѐ-џ → Ѐ-Џ
        if (cp == 0x0451) return 0x0401U;                     // ё → Ё
        if (in_range(cp, 0x0561, 0x0586)) return cp - 0x30U;  // армянские
        if (in_range(cp, 0x24D0, 0x24E9)) return cp - 0x1AU;  // ⓐ → Ⓐ
        if (in_range(cp, 0xFF41, 0xFF5A)) return cp - 0x20U;  // полноширинные
        if (cp == 0x04CF) return 0x04C0U;                     // ӏ → Ӏ
    } else {
        if (in_range(cp, 0x00C0, 0x00DE) && cp != 0x00D7) return cp + 0x20U;
        if (cp == 0x0178U) return 0x00FFU;
        if (cp == 0x039C) return 0x00B5U;                     // Μ → µ
        if (cp == 0x0130) return 0x0069U;                     // İ → i
        if (in_range(cp, 0x0391, 0x03A1)) return cp + 0x20U;  // Α-Ρ → α-ρ
        if (in_range(cp, 0x03A3, 0x03AB)) return cp + 0x20U;  // Σ-Ϋ → σ-ϋ
        if (in_range(cp, 0x0410, 0x042F)) return cp + 0x20U;  // А-Я → а-я
        if (in_range(cp, 0x0400, 0x040F)) return cp + 0x50U;  // Ѐ-Џ → ѐ-џ
        if (cp == 0x0401) return 0x0451U;                     // Ё → ё
        if (in_range(cp, 0x0531, 0x0556)) return cp + 0x30U;
        if (in_range(cp, 0x24B6, 0x24CF)) return cp + 0x1AU;
        if (in_range(cp, 0xFF21, 0xFF3A)) return cp + 0x20U;
        if (cp == 0x04C0) return 0x04CFU;
    }

    // Парные диапазоны (латиница-ext, кириллица-ext, коптский, чероки…).
    for (const auto& range : kEvenOddUpper) {
        if (cp < range.lo || cp > range.hi) continue;
        const bool is_upper = range.lo_is_upper ? ((cp - range.lo) % 2U == 0U)
                                                : ((cp - range.lo) % 2U != 0U);
        if (to_upper) {
            if (is_upper) return cp;
            return cp - 1U;
        }
        if (!is_upper) return cp;
        return cp + 1U;
    }

    // Полноширинные латинские буквы уже обработаны выше; остальное — без изменений.
    return cp;
}

/// Базовые буквы для снятия диакритики: подмножество Latin-1 и Latin Extended-A.
[[nodiscard]] const char* accent_base(std::uint32_t cp) noexcept {
    switch (cp) {
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
            return "A";
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
            return "a";
        case 0x00C7: return "C";
        case 0x00E7: return "c";
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: return "E";
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: return "e";
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: return "I";
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: return "i";
        case 0x00D1: return "N";
        case 0x00F1: return "n";
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
            return "O";
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
            return "o";
        case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: return "U";
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: return "u";
        case 0x00DD: return "Y";
        case 0x00FD: case 0x00FF: return "y";
        case 0x0100: case 0x0102: case 0x0104: return "A";
        case 0x0101: case 0x0103: case 0x0105: return "a";
        case 0x0106: case 0x0108: case 0x010A: case 0x010C: return "C";
        case 0x0107: case 0x0109: case 0x010B: case 0x010D: return "c";
        case 0x010E: case 0x0110: return "D";
        case 0x010F: case 0x0111: return "d";
        case 0x0112: case 0x0114: case 0x0116: case 0x0118: return "E";
        case 0x0113: case 0x0115: case 0x0117: case 0x0119: return "e";
        case 0x011A: case 0x011C: case 0x011E: case 0x0120: case 0x0122: return "G";
        case 0x011B: case 0x011D: case 0x011F: case 0x0121: case 0x0123: return "g";
        case 0x0124: case 0x0126: return "H";
        case 0x0125: case 0x0127: return "h";
        case 0x0128: case 0x012A: case 0x012C: case 0x012E: case 0x0130: return "I";
        case 0x0129: case 0x012B: case 0x012D: case 0x012F: case 0x0131: return "i";
        case 0x0134: return "J";
        case 0x0135: return "j";
        case 0x0136: return "K";
        case 0x0137: case 0x0138: return "k";
        case 0x0139: case 0x013B: case 0x013D: return "L";
        case 0x013A: case 0x013C: case 0x013E: return "l";
        case 0x0143: case 0x0145: case 0x0147: return "N";
        case 0x0144: case 0x0146: case 0x0148: return "n";
        case 0x014C: case 0x014E: case 0x0150: return "O";
        case 0x014D: case 0x014F: case 0x0151: return "o";
        case 0x0154: case 0x0156: case 0x0158: return "R";
        case 0x0155: case 0x0157: case 0x0159: return "r";
        case 0x015A: case 0x015C: case 0x015E: case 0x0160: return "S";
        case 0x015B: case 0x015D: case 0x015F: case 0x0161: return "s";
        case 0x0162: case 0x0164: case 0x0166: return "T";
        case 0x0163: case 0x0165: case 0x0167: return "t";
        case 0x0168: case 0x016A: case 0x016C: case 0x016E: case 0x0170: case 0x0172:
            return "U";
        case 0x0169: case 0x016B: case 0x016D: case 0x016F: case 0x0171: case 0x0173:
            return "u";
        case 0x0174: return "W";
        case 0x0175: return "w";
        case 0x0176: case 0x0178: return "Y";
        case 0x0179: case 0x017B: case 0x017D: return "Z";
        case 0x017A: case 0x017C: case 0x017E: return "z";
        // Кириллические «ё» и украинские «ї/є/ґ» — тоже удобно приводить к базовой букве.
        case 0x0451: return "е";
        case 0x0401: return "Е";
        case 0x0457: return "і";
        case 0x0407: return "І";
        case 0x0454: return "е";
        case 0x0404: return "Е";
        case 0x0491: return "г";
        case 0x0490: return "Г";
        case 0x045E: return "у";
        case 0x040E: return "У";
        default: return nullptr;
    }
}

/// Транслитерация кириллицы (ГОСТ 7.79-2000, система Б).
[[nodiscard]] const char* translit_ru(std::uint32_t cp) noexcept {
    switch (cp) {
        case 0x0430: return "a";   case 0x0410: return "A";
        case 0x0431: return "b";   case 0x0411: return "B";
        case 0x0432: return "v";   case 0x0412: return "V";
        case 0x0433: return "g";   case 0x0413: return "G";
        case 0x0434: return "d";   case 0x0414: return "D";
        case 0x0435: return "e";   case 0x0415: return "E";
        case 0x0451: return "e";   case 0x0401: return "E";
        case 0x0436: return "zh";  case 0x0416: return "Zh";
        case 0x0437: return "z";   case 0x0417: return "Z";
        case 0x0438: return "i";   case 0x0418: return "I";
        case 0x0439: return "j";   case 0x0419: return "J";
        case 0x043A: return "k";   case 0x041A: return "K";
        case 0x043B: return "l";   case 0x041B: return "L";
        case 0x043C: return "m";   case 0x041C: return "M";
        case 0x043D: return "n";   case 0x041D: return "N";
        case 0x043E: return "o";   case 0x041E: return "O";
        case 0x043F: return "p";   case 0x041F: return "P";
        case 0x0440: return "r";   case 0x0420: return "R";
        case 0x0441: return "s";   case 0x0421: return "S";
        case 0x0442: return "t";   case 0x0422: return "T";
        case 0x0443: return "u";   case 0x0423: return "U";
        case 0x0444: return "f";   case 0x0424: return "F";
        case 0x0445: return "kh";  case 0x0425: return "Kh";
        case 0x0446: return "ts";  case 0x0426: return "Ts";
        case 0x0447: return "ch";  case 0x0427: return "Ch";
        case 0x0448: return "sh";  case 0x0428: return "Sh";
        case 0x0449: return "shch"; case 0x0429: return "Shch";
        case 0x044A: return "";    case 0x042A: return "";
        case 0x044B: return "y";   case 0x042B: return "Y";
        case 0x044C: return "";    case 0x042C: return "";
        case 0x044D: return "e";   case 0x042D: return "E";
        case 0x044E: return "yu";  case 0x042E: return "Yu";
        case 0x044F: return "ya";  case 0x042F: return "Ya";
        case 0x0456: return "i";   case 0x0406: return "I";
        case 0x0457: return "i";   case 0x0407: return "I";
        case 0x0454: return "ie";  case 0x0404: return "Ie";
        case 0x0491: return "g";   case 0x0490: return "G";
        default: return nullptr;
    }
}

/// Таблица CP1251 → Unicode для байтов 0x80..0xFF.
constexpr std::array<std::uint32_t, 128> kCp1251High = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447,
    0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

}  // namespace

std::string Utf8Error::to_string() const {
    std::string out = "позиция ";
    out += std::to_string(offset);
    out += ": ";
    out += reason;
    return out;
}

bool utf8_is_valid(std::string_view s, Utf8Error* error) {
    std::size_t offset = 0;
    while (offset < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[offset]);
        std::size_t length = 0;
        if (b0 < 0x80U) {
            length = 1;
        } else if (b0 >= 0xC2U && b0 <= 0xDFU) {
            length = 2;
        } else if (b0 >= 0xE0U && b0 <= 0xEFU) {
            length = 3;
        } else if (b0 >= 0xF0U && b0 <= 0xF4U) {
            length = 4;
        } else {
            if (error) {
                error->offset = offset;
                error->reason = (b0 >= 0x80U && b0 <= 0xBFU)
                                    ? "байт-продолжение вне последовательности"
                                    : "недопустимый ведущий байт";
            }
            return false;
        }
        if (offset + length > s.size()) {
            if (error) {
                error->offset = offset;
                error->reason = "обрыв последовательности на конце строки";
            }
            return false;
        }
        for (std::size_t i = 1; i < length; ++i) {
            const auto bc = static_cast<unsigned char>(s[offset + i]);
            if ((bc & 0xC0U) != 0x80U) {
                if (error) {
                    // Указываем НАЧАЛО неверной последовательности: пользователю нужно
                    // найти её в файле, а не отдельный «плохой» байт внутри.
                    error->offset = offset;
                    error->reason = "ожидался байт-продолжение (сдвиг " + std::to_string(i) + ")";
                }
                return false;
            }
        }
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (cp == kReplacement && length != 1) {
            if (error) {
                error->offset = offset;
                error->reason = "недопустимая последовательность (переполнение или суррогат)";
            }
            return false;
        }
        offset += consumed == 0 ? 1 : consumed;
    }
    if (error) *error = Utf8Error{};
    return true;
}

std::vector<std::uint32_t> utf8_decode(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    std::size_t offset = 0;
    while (offset < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        out.push_back(cp);
        offset += consumed;
    }
    return out;
}

void utf8_append(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
        out.push_back(static_cast<char>(codepoint));
        return;
    }
    std::uint32_t cp = codepoint;
    if (cp > kMaxCodepoint || in_range(cp, kSurrogateFirst, kSurrogateLast)) cp = kReplacement;
    if (cp <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        return;
    }
    if (cp <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        return;
    }
    out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
}

std::string utf8_encode(std::uint32_t codepoint) {
    std::string out;
    utf8_append(out, codepoint);
    return out;
}

std::string utf8_from(const std::vector<std::uint32_t>& codepoints) {
    std::string out;
    out.reserve(codepoints.size() + codepoints.size() / 2);
    for (std::uint32_t cp : codepoints) utf8_append(out, cp);
    return out;
}

std::size_t utf8_length(std::string_view s) noexcept {
    std::size_t count = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++count;
    }
    return count;
}

std::size_t utf8_byte_offset(std::string_view s, std::size_t index) noexcept {
    std::size_t seen = 0;
    std::size_t offset = 0;
    while (offset < s.size() && seen < index) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        offset += consumed;
        ++seen;
    }
    return offset;
}

std::size_t utf8_codepoint_at(std::string_view s, std::size_t index) noexcept {
    const std::size_t offset = utf8_byte_offset(s, index);
    if (offset >= s.size()) return 0;
    std::uint32_t cp = 0;
    if (utf8_next(s, offset, &cp) == 0) return 0;
    return cp;
}

std::string_view utf8_substr(std::string_view s, std::size_t start, std::size_t count) noexcept {
    const std::size_t begin = utf8_byte_offset(s, start);
    if (begin >= s.size()) return {};
    if (count == kNpos) return s.substr(begin);
    const std::size_t end = utf8_byte_offset(s, start + count);
    return s.substr(begin, end - begin);
}

std::size_t utf8_offset_to_line_column(std::string_view s, std::size_t byte_offset, std::size_t* line,
                                       std::size_t* column) noexcept {
    const std::size_t limit = clamp_index(byte_offset, s.size());
    std::size_t current_line = 1;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (s[i] == '\n') {
            ++current_line;
            line_start = i + 1;
        }
    }
    if (line) *line = current_line;
    if (column) *column = utf8_length(s.substr(line_start, limit - line_start)) + 1;
    return current_line;
}

std::u16string utf8_to_utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    std::size_t offset = 0;
    while (offset < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        offset += consumed;
        if (cp < 0x10000U) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            const std::uint32_t value = cp - 0x10000U;
            out.push_back(static_cast<char16_t>(0xD800U + (value >> 10U)));
            out.push_back(static_cast<char16_t>(0xDC00U + (value & 0x3FFU)));
        }
    }
    return out;
}

std::string utf16_to_utf8(std::u16string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const std::uint32_t unit = s[i];
        if (in_range(unit, 0xD800U, 0xDBFFU) && i + 1 < s.size()) {
            const std::uint32_t low = s[i + 1];
            if (in_range(low, 0xDC00U, 0xDFFFU)) {
                const std::uint32_t cp = 0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U);
                utf8_append(out, cp);
                ++i;
                continue;
            }
        }
        if (in_range(unit, 0xD800U, 0xDFFFU)) {
            utf8_append(out, kReplacement);
            continue;
        }
        utf8_append(out, unit);
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if constexpr (sizeof(wchar_t) >= 4) {
        std::wstring out;
        out.reserve(s.size());
        for (std::uint32_t cp : utf8_decode(s)) out.push_back(static_cast<wchar_t>(cp));
        return out;
    } else {
        const std::u16string utf16 = utf8_to_utf16(s);
        return std::wstring(utf16.begin(), utf16.end());
    }
}

std::string wide_to_utf8(std::wstring_view s) {
    if constexpr (sizeof(wchar_t) >= 4) {
        std::string out;
        out.reserve(s.size() * 2);
        for (wchar_t wc : s) utf8_append(out, static_cast<std::uint32_t>(wc));
        return out;
    } else {
        std::u16string utf16;
        utf16.reserve(s.size());
        for (wchar_t wc : s) utf16.push_back(static_cast<char16_t>(wc));
        return utf16_to_utf8(utf16);
    }
}

std::string cp1251_to_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (char c : s) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80U) {
            out.push_back(c);
        } else {
            utf8_append(out, kCp1251High[byte - 0x80U]);
        }
    }
    return out;
}

std::string utf8_to_cp1251(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::uint32_t cp : utf8_decode(s)) {
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
            continue;
        }
        bool found = false;
        for (std::size_t i = 0; i < kCp1251High.size(); ++i) {
            if (kCp1251High[i] == cp) {
                out.push_back(static_cast<char>(0x80U + i));
                found = true;
                break;
            }
        }
        if (!found) out.push_back('?');
    }
    return out;
}

std::string_view category_name(Category c) noexcept {
    switch (c) {
        case Category::Other:       return "other";
        case Category::Control:     return "control";
        case Category::Space:       return "space";
        case Category::Digit:       return "digit";
        case Category::Letter:      return "letter";
        case Category::Mark:        return "mark";
        case Category::Punctuation: return "punctuation";
        case Category::Symbol:      return "symbol";
        case Category::Separator:   return "separator";
    }
    return "other";
}

bool is_space_cp(std::uint32_t cp) noexcept {
    if (cp < 0x80U) return is_ascii_space(static_cast<char>(cp));
    switch (cp) {
        case 0x00A0: case 0x1680: case 0x2028: case 0x2029: case 0x202F:
        case 0x205F: case 0x3000: case 0xFEFF:
            return true;
        default:
            return in_range(cp, 0x2000, 0x200A);
    }
}

bool is_digit_cp(std::uint32_t cp) noexcept {
    if (cp < 0x80U) return cp >= '0' && cp <= '9';
    return in_range(cp, 0x0660, 0x0669) || in_range(cp, 0x06F0, 0x06F9) ||
           in_range(cp, 0x0966, 0x096F) || in_range(cp, 0x09E6, 0x09EF) ||
           in_range(cp, 0x0A66, 0x0A6F) || in_range(cp, 0x0AE6, 0x0AEF) ||
           in_range(cp, 0x0BE6, 0x0BEF) || in_range(cp, 0x0C66, 0x0C6F) ||
           in_range(cp, 0x0E50, 0x0E59) || in_range(cp, 0x0ED0, 0x0ED9) ||
           in_range(cp, 0xFF10, 0xFF19);
}

bool is_combining_cp(std::uint32_t cp) noexcept {
    if (cp < 0x0300U) return false;
    return in_ranges(cp, kCombining, sizeof(kCombining) / sizeof(kCombining[0]));
}

bool is_letter_cp(std::uint32_t cp) noexcept {
    if (cp < 0x80U) return is_ascii_alpha(static_cast<char>(cp));
    switch (cp) {
        case 0x00AA: case 0x00B5: case 0x00BA: case 0x00DF: case 0x00E6: case 0x00F0:
        case 0x00F8: case 0x00FE: case 0x00FF: case 0x0138: case 0x0149: case 0x017F:
            return true;
        default: break;
    }
    if (in_range(cp, 0x00C0, 0x024F)) return true;      // латиница с диакритикой
    if (in_range(cp, 0x0370, 0x03FF)) return true;      // греческий
    if (in_range(cp, 0x0400, 0x052F)) return true;      // кириллица
    if (in_range(cp, 0x0531, 0x058F)) return true;      // армянский
    if (in_range(cp, 0x0590, 0x05FF)) return true;      // иврит
    if (in_range(cp, 0x0600, 0x06FF)) return true;      // арабский
    if (in_range(cp, 0x0900, 0x0DFF)) return true;      // индийские письменности
    if (in_range(cp, 0x0E00, 0x0E7F)) return true;      // тайский
    if (in_range(cp, 0x10A0, 0x10FF)) return true;      // грузинский
    if (in_range(cp, 0x1100, 0x11FF)) return true;      // хангыль (jamo)
    if (in_range(cp, 0x1E00, 0x1FFF)) return true;      // расширенная латиница/греческий
    if (in_range(cp, 0x2C00, 0x2DFF)) return true;
    if (in_range(cp, 0x3040, 0x30FF)) return true;      // хирагана/катакана
    if (in_range(cp, 0x3400, 0x4DBF)) return true;      // CJK ext A
    if (in_range(cp, 0x4E00, 0x9FFF)) return true;      // CJK
    if (in_range(cp, 0xA640, 0xA69F)) return true;      // кириллица-ext
    if (in_range(cp, 0xAC00, 0xD7A3)) return true;      // хангыль слоги
    if (in_range(cp, 0xF900, 0xFAFF)) return true;      // CJK совместимость
    if (in_range(cp, 0xFF21, 0xFF3A) || in_range(cp, 0xFF41, 0xFF5A)) return true;
    if (in_range(cp, 0x1F600, 0x1F64F)) return false;   // эмодзи — не буквы
    return in_ranges(cp, kLettersExtra, sizeof(kLettersExtra) / sizeof(kLettersExtra[0]));
}

bool is_word_cp(std::uint32_t cp) noexcept {
    return is_letter_cp(cp) || is_digit_cp(cp) || cp == '_' || cp == '-' ||
           in_range(cp, 0x0300, 0x036F);
}

bool is_latin_cp(std::uint32_t cp) noexcept {
    return in_ranges(cp, kLatin, sizeof(kLatin) / sizeof(kLatin[0]));
}

bool is_cyrillic_cp(std::uint32_t cp) noexcept {
    return in_ranges(cp, kCyrillic, sizeof(kCyrillic) / sizeof(kCyrillic[0]));
}

bool is_greek_cp(std::uint32_t cp) noexcept {
    return in_ranges(cp, kGreek, sizeof(kGreek) / sizeof(kGreek[0]));
}

bool is_cjk_cp(std::uint32_t cp) noexcept {
    return in_ranges(cp, kCjk, sizeof(kCjk) / sizeof(kCjk[0]));
}

bool is_emoji_cp(std::uint32_t cp) noexcept {
    return in_ranges(cp, kEmoji, sizeof(kEmoji) / sizeof(kEmoji[0]));
}

bool is_private_use_cp(std::uint32_t cp) noexcept {
    return in_range(cp, 0xE000, 0xF8FF) || in_range(cp, 0xF0000, 0xFFFFD) ||
           in_range(cp, 0x100000, 0x10FFFD);
}

std::string_view script_of(std::uint32_t cp) noexcept {
    if (is_cyrillic_cp(cp)) return "cyrillic";
    if (is_greek_cp(cp)) return "greek";
    if (is_cjk_cp(cp)) return "cjk";
    if (is_latin_cp(cp)) return "latin";
    if (cp >= 0x0590U && cp <= 0x05FFU) return "hebrew";
    if (cp >= 0x0600U && cp <= 0x06FFU) return "arabic";
    if (cp >= 0x0900U && cp <= 0x0DFFU) return "indic";
    if (cp >= 0x0E00U && cp <= 0x0E7FU) return "thai";
    if (is_emoji_cp(cp)) return "emoji";
    if (is_digit_cp(cp)) return "digit";
    if (is_space_cp(cp)) return "space";
    return "unknown";
}

Category category_of(std::uint32_t cp) noexcept {
    if (cp < 0x80U) {
        const char c = static_cast<char>(cp);
        if (cp < 0x20U || cp == 0x7FU) return Category::Control;
        if (is_ascii_space(c)) return Category::Space;
        if (is_ascii_digit(c)) return Category::Digit;
        if (is_ascii_alpha(c)) return Category::Letter;
        if (in_ranges(cp, kPunctuation, sizeof(kPunctuation) / sizeof(kPunctuation[0]))) {
            return Category::Punctuation;
        }
        if (in_ranges(cp, kSymbols, sizeof(kSymbols) / sizeof(kSymbols[0]))) return Category::Symbol;
        return Category::Other;
    }
    if (is_combining_cp(cp)) return Category::Mark;
    if (is_space_cp(cp)) return Category::Space;
    if (cp == 0x00A0U) return Category::Space;
    if (is_digit_cp(cp)) return Category::Digit;
    if (is_letter_cp(cp)) return Category::Letter;
    if (in_ranges(cp, kPunctuation, sizeof(kPunctuation) / sizeof(kPunctuation[0]))) {
        return Category::Punctuation;
    }
    if (in_ranges(cp, kSymbols, sizeof(kSymbols) / sizeof(kSymbols[0]))) return Category::Symbol;
    if (in_range(cp, 0x2000U, 0x200FU)) return Category::Separator;
    if (in_range(cp, 0x2028U, 0x2029U)) return Category::Separator;
    if (in_range(cp, 0x202AU, 0x202EU)) return Category::Control;
    if (in_range(cp, 0x2060U, 0x206FU)) return Category::Control;
    if (in_range(cp, 0xFFF9U, 0xFFFBU)) return Category::Control;
    return Category::Other;
}

std::uint32_t to_lower_cp(std::uint32_t cp) noexcept { return case_shift(cp, false); }
std::uint32_t to_upper_cp(std::uint32_t cp) noexcept { return case_shift(cp, true); }

std::string lower_unicode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t offset = 0;
    while (offset < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        utf8_append(out, to_lower_cp(cp));
        offset += consumed;
    }
    return out;
}

std::string upper_unicode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t offset = 0;
    while (offset < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        utf8_append(out, to_upper_cp(cp));
        offset += consumed;
    }
    return out;
}

std::string capitalize(std::string_view s) {
    const std::size_t first = first_codepoint_length(s);
    if (first == 0) return {};
    return upper_unicode(s.substr(0, first)) + lower_unicode(s.substr(first));
}

std::string title_case(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool at_word_start = true;
    std::size_t offset = 0;
    while (offset < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        offset += consumed;
        if (is_word_cp(cp)) {
            utf8_append(out, at_word_start ? to_upper_cp(cp) : to_lower_cp(cp));
            at_word_start = false;
        } else {
            utf8_append(out, cp);
            at_word_start = true;
        }
    }
    return out;
}

int codepoint_width(std::uint32_t cp) noexcept {
    if (cp == 0) return 0;
    if (in_ranges(cp, kZeroWidth, sizeof(kZeroWidth) / sizeof(kZeroWidth[0]))) return 0;
    if (is_combining_cp(cp)) return 0;
    if (cp == 0x200DU || cp == 0x200CU) return 0;     // ZWJ / ZWNJ
    if (in_ranges(cp, kWide, sizeof(kWide) / sizeof(kWide[0]))) return 2;
    if (cp < 0x20U) return 0;
    return 1;
}

int display_width(std::string_view s) noexcept {
    int width = 0;
    std::size_t offset = 0;
    while (offset < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        offset += consumed;
        width += codepoint_width(cp);
    }
    return width;
}

std::size_t grapheme_count(std::string_view s) noexcept {
    std::size_t count = 0;
    std::size_t offset = 0;
    bool pending_base = false;
    while (offset < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        offset += consumed;
        if (is_combining_cp(cp) || cp == 0x200DU) {
            // Знак присоединяется к предыдущему символу.
            continue;
        }
        if (!pending_base) {
            ++count;
            pending_base = true;
        } else {
            ++count;
        }
    }
    return count;
}

std::string remove_accents(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    const auto codepoints = utf8_decode(s);
    for (std::uint32_t cp : codepoints) {
        if (is_combining_cp(cp)) continue;
        if (const char* base = accent_base(cp)) {
            out.append(base);
            continue;
        }
        utf8_append(out, cp);
    }
    return out;
}

std::string transliterate(std::string_view s) {
    std::string out;
    out.reserve(s.size() * 2);
    for (std::uint32_t cp : utf8_decode(s)) {
        if (const char* mapped = translit_ru(cp)) {
            out.append(mapped);
            continue;
        }
        if (is_cyrillic_cp(cp)) {
            out.append("?");
            continue;
        }
        utf8_append(out, cp);
    }
    return out;
}

std::string utf8_truncate(std::string_view s, int max_width, std::string_view ellipsis) {
    if (max_width <= 0) return {};
    if (display_width(s) <= max_width) return std::string(s);

    const int ellipsis_width = display_width(ellipsis);
    if (ellipsis_width >= max_width) {
        // Даже многоточие не влезает — режем как есть по колонкам.
        const auto slice = utf8_slice_columns(s, 0, max_width);
        return std::string(slice);
    }

    const auto slice = utf8_slice_columns(s, 0, max_width - ellipsis_width);
    std::string out(slice);
    out.append(ellipsis);
    return out;
}

std::string_view utf8_slice_columns(std::string_view s, int start_column, int width) noexcept {
    if (width <= 0) return {};
    int column = 0;
    std::size_t begin = s.size();
    std::size_t offset = 0;
    while (offset < s.size()) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        const int cp_width = codepoint_width(cp);
        if (column >= start_column && begin == s.size()) begin = offset;
        if (column + cp_width > start_column + width) break;
        column += cp_width;
        offset += consumed;
    }
    if (begin == s.size()) return {};
    return s.substr(begin, offset - begin);
}

std::string highlight_position(std::string_view s, std::size_t codepoint_index,
                               std::string_view marker) {
    const std::size_t offset = utf8_byte_offset(s, codepoint_index);
    int column = 0;
    for (std::size_t i = 0; i < offset;) {
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, i, &cp);
        if (consumed == 0) break;
        column += codepoint_width(cp);
        i += consumed;
    }
    const std::string first = std::string(s.substr(0, offset));
    const std::string last = std::string(s.substr(offset));
    std::string out = first;
    out += std::string(static_cast<std::size_t>(column), ' ');
    out += marker;
    out += "\n";
    out += first;
    out += last;
    return out;
}

std::optional<std::size_t> find_nocase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return 0;
    const std::string folded_haystack = lower_unicode(haystack);
    const std::string folded_needle = lower_unicode(needle);
    const std::size_t found = folded_haystack.find(folded_needle);
    if (found == kNpos) return std::nullopt;
    return found;
}



// ===========================================================================
//  4. ANSI: цвета и стили
//
//  Терминал — основной интерфейс агента, поэтому раскраска и, главное,
//  корректное измерение ширины цветных строк (для таблиц и выравнивания)
//  живут здесь, а не в UI-модуле: ими пользуется и рендер, и diff, и логи.
// ===========================================================================

namespace {


constexpr char kEsc = '\x1b';

/// Проверяет, что строка начинается с ANSI-последовательности CSI, и возвращает её длину.
[[nodiscard]] std::size_t ansi_sequence_length(std::string_view s, std::size_t offset) noexcept {
    if (offset + 1 >= s.size() || s[offset] != kEsc) return 0;
    const char next = s[offset + 1];
    if (next == '[') {
        std::size_t i = offset + 2;
        while (i < s.size()) {
            const auto c = static_cast<unsigned char>(s[i]);
            if (c >= 0x40U && c <= 0x7EU) return i - offset + 1;
            ++i;
        }
        return s.size() - offset;
    }
    if (next == ']') {                                  // OSC: ...BEL либо ESC-«закрывающая скобка»
        std::size_t i = offset + 2;
        while (i < s.size()) {
            if (s[i] == '\a') return i - offset + 1;
            if (s[i] == kEsc && i + 1 < s.size() && s[i + 1] == '\\') return i - offset + 2;
            ++i;
        }
        return s.size() - offset;
    }
    return 2;
}

}  // namespace

std::string colorize(std::string_view text, std::string_view code) {
    if (code.empty() || text.empty()) return std::string(text);
    std::string out;
    out.reserve(code.size() + text.size() + ansi::reset.size());
    out.append(code);
    out.append(text);
    out.append(ansi::reset);
    return out;
}

std::string rgb_colorize(std::string_view text, int r, int g, int b) {
    const int rr = std::min(std::max(r, 0), 255);
    const int gg = std::min(std::max(g, 0), 255);
    const int bb = std::min(std::max(b, 0), 255);
    std::string code = "\x1b[38;2;";
    code += std::to_string(rr);
    code += ';';
    code += std::to_string(gg);
    code += ';';
    code += std::to_string(bb);
    code += 'm';
    return colorize(text, code);
}

std::string bold_text(std::string_view s) { return colorize(s, ansi::bold); }
std::string dim_text(std::string_view s) { return colorize(s, ansi::dim); }
std::string red_text(std::string_view s) { return colorize(s, ansi::red); }
std::string green_text(std::string_view s) { return colorize(s, ansi::green); }
std::string yellow_text(std::string_view s) { return colorize(s, ansi::yellow); }
std::string blue_text(std::string_view s) { return colorize(s, ansi::blue); }
std::string cyan_text(std::string_view s) { return colorize(s, ansi::cyan); }
std::string magenta_text(std::string_view s) { return colorize(s, ansi::magenta); }
std::string gray_text(std::string_view s) { return colorize(s, ansi::gray); }

bool has_ansi(std::string_view s) noexcept {
    return s.find(kEsc) != kNpos;
}

std::string strip_ansi(std::string_view s) {
    if (!has_ansi(s)) return std::string(s);
    std::string out;
    out.reserve(s.size());
    std::size_t offset = 0;
    while (offset < s.size()) {
        if (s[offset] == kEsc) {
            const std::size_t length = ansi_sequence_length(s, offset);
            if (length > 0) {
                offset += length;
                continue;
            }
        }
        out.push_back(s[offset]);
        ++offset;
    }
    return out;
}

int terminal_width(std::string_view s) noexcept {
    if (!has_ansi(s)) return display_width(s);
    return display_width(strip_ansi(s));
}

std::string truncate_visible(std::string_view s, int max_width, std::string_view ellipsis) {
    if (max_width <= 0) return {};
    if (terminal_width(s) <= max_width) return std::string(s);

    const int ellipsis_width = display_width(ellipsis);
    const int budget = std::max(0, max_width - ellipsis_width);

    std::string out;
    int width = 0;
    std::size_t offset = 0;
    while (offset < s.size()) {
        if (s[offset] == kEsc) {
            const std::size_t length = ansi_sequence_length(s, offset);
            if (length > 0) {
                out.append(s.substr(offset, length));
                offset += length;
                continue;
            }
        }
        std::uint32_t cp = 0;
        const std::size_t consumed = utf8_next(s, offset, &cp);
        if (consumed == 0) break;
        if (width + codepoint_width(cp) > budget) break;
        width += codepoint_width(cp);
        out.append(s.substr(offset, consumed));
        offset += consumed;
    }
    if (has_ansi(out)) out.append(ansi::reset);
    out.append(ellipsis);
    return out;
}

// ===========================================================================
//  5. Экранирование и кодирование
// ===========================================================================

namespace {

[[nodiscard]] bool is_printable_ascii(char c) noexcept {
    const auto byte = static_cast<unsigned char>(c);
    return byte >= 0x20U && byte < 0x7FU;
}

[[nodiscard]] std::string hex_byte(unsigned value, bool uppercase) {
    const std::string_view digits = hex_digits(uppercase);
    std::string out;
    out.push_back(digits[(value >> 4U) & 0x0FU]);
    out.push_back(digits[value & 0x0FU]);
    return out;
}

/// Разбирает \uXXXX (с учётом суррогатных пар) в готовую UTF-8 последовательность.
[[nodiscard]] std::size_t decode_unicode_escape(std::string_view s, std::size_t offset,
                                                std::string& out) {
    auto read_hex4 = [&s](std::size_t at, std::uint32_t* value) -> bool {
        if (at + 4 > s.size()) return false;
        std::uint32_t result = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const int digit = ascii_digit_value(s[at + i]);
            if (digit < 0) return false;
            result = (result << 4U) | static_cast<std::uint32_t>(digit);
        }
        *value = result;
        return true;
    };

    std::uint32_t first = 0;
    if (!read_hex4(offset, &first)) return 0;

    if (in_range(first, 0xD800U, 0xDBFFU) && offset + 6 + 4 <= s.size() && s[offset + 4] == '\\' &&
        s[offset + 5] == 'u') {
        std::uint32_t second = 0;
        if (read_hex4(offset + 6, &second) && in_range(second, 0xDC00U, 0xDFFFU)) {
            const std::uint32_t cp = 0x10000U + ((first - 0xD800U) << 10U) + (second - 0xDC00U);
            utf8_append(out, cp);
            return 10;
        }
    }
    utf8_append(out, first);
    return 4;
}

/// Ищем «первую половину» байта в строке: используется при разборе %XX.
[[nodiscard]] int hex_pair_value(char hi, char lo) noexcept {
    const int a = ascii_digit_value(hi);
    const int b = ascii_digit_value(lo);
    if (a < 0 || b < 0) return -1;
    return (a << 4) | b;
}

constexpr std::array<std::string_view, 10> kHtmlNamed = {
    "&amp;", "&lt;", "&gt;", "&quot;", "&#39;", "&nbsp;", "&copy;", "&mdash;", "&ndash;", "&hellip;",
};
constexpr std::array<std::string_view, 10> kHtmlReplacement = {
    "&", "<", ">", "\"", "'", "\xC2\xA0", "©", "—", "–", "…",
};

/// Индекс в таблице разделителей shell, которые требуют кавычек.
/// Текст в виде последовательности кодпоинтов. Для ASCII — просто байты.
[[nodiscard]] std::vector<std::uint32_t> codepoints_of(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    bool ascii_only = true;
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x80U) {
            ascii_only = false;
            break;
        }
    }
    if (ascii_only) {
        for (char c : s) out.push_back(static_cast<std::uint32_t>(static_cast<unsigned char>(c)));
        return out;
    }
    for (std::uint32_t cp : utf8_decode(s)) out.push_back(cp);
    return out;
}

/// Левенштейн по кодпоинтам (внутренняя реализация).
[[nodiscard]] std::size_t levenshtein_cp(const std::vector<std::uint32_t>& a,
                                         const std::vector<std::uint32_t>& b,
                                         std::size_t limit) {
    if (a.empty()) return b.size();
    if (b.empty()) return a.size();
    const std::vector<std::uint32_t>* left = &a;
    const std::vector<std::uint32_t>* right = &b;
    if (left->size() > right->size()) std::swap(left, right);

    std::vector<std::size_t> previous(left->size() + 1);
    std::vector<std::size_t> current(left->size() + 1);
    for (std::size_t i = 0; i <= left->size(); ++i) previous[i] = i;

    for (std::size_t j = 1; j <= right->size(); ++j) {
        current[0] = j;
        std::size_t row_min = current[0];
        for (std::size_t i = 1; i <= left->size(); ++i) {
            const std::size_t cost = ((*left)[i - 1] == (*right)[j - 1]) ? 0 : 1;
            current[i] = std::min({previous[i] + 1, current[i - 1] + 1, previous[i - 1] + cost});
            row_min = std::min(row_min, current[i]);
        }
        if (limit != kNpos && row_min > limit) return limit + 1;
        std::swap(previous, current);
    }
    const std::size_t result = previous[left->size()];
    if (limit != kNpos && result > limit) return limit + 1;
    return result;
}

/// Дамерау–Левенштейн по кодпоинтам: учитывает перестановку соседних символов.
/// Матрица хранится в плоском буфере — одна аллокация вместо (n+2) вложенных.
#if defined(__GNUC__) && !defined(__clang__)
// GCC 12 выдаёт здесь -Wnull-dereference: ложное срабатывание анализатора на
// индексации буфера, который выделен выше и никогда не бывает пустым (n,m >= 1).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif
[[nodiscard]] std::size_t damerau_cp(const std::vector<std::uint32_t>& a,
                                     const std::vector<std::uint32_t>& b, std::size_t limit) {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    if (n == 0) return m;
    if (m == 0) return n;

    const std::size_t width = m + 2;
    const std::size_t kInf = kNpos;
    const std::size_t max_distance = n + m;

    // Матрица в плоском буфере: одна аллокация, а рабочий указатель избавляет от
    // ложных срабатываний -Wnull-dereference, которые GCC 12 выдаёт на вложенные
    // обращения к std::vector внутри горячих циклов динамического программирования.
    std::vector<std::size_t> storage((n + 2) * width, 0);
    std::size_t* const d = storage.data();
    const auto at = [width](std::size_t i, std::size_t j) noexcept { return i * width + j; };

    d[at(0, 0)] = kInf;
    for (std::size_t i = 0; i <= n; ++i) {
        d[at(i + 1, 0)] = kInf;
        d[at(i + 1, 1)] = i;
    }
    for (std::size_t j = 0; j <= m; ++j) {
        d[at(0, j + 1)] = kInf;
        d[at(1, j + 1)] = j;
    }

    std::map<std::uint32_t, std::size_t> last_row;
    for (std::size_t i = 1; i <= n; ++i) {
        std::size_t last_match_column = 0;
        for (std::size_t j = 1; j <= m; ++j) {
            const auto found = last_row.find(b[j - 1]);
            const std::size_t i1 = found == last_row.end() ? 0 : found->second;
            const std::size_t j1 = last_match_column;
            std::size_t cost = 1;
            if (a[i - 1] == b[j - 1]) {
                cost = 0;
                last_match_column = j;
            }
            const std::size_t substitution = d[at(i, j)] + cost;
            const std::size_t insertion = d[at(i + 1, j)] + 1;
            const std::size_t deletion = d[at(i, j + 1)] + 1;
            const std::size_t transposition =
                (i1 > 0 && j1 > 0) ? d[at(i1, j1)] + (i - i1 - 1) + 1 + (j - j1 - 1) : max_distance;
            d[at(i + 1, j + 1)] = std::min({substitution, insertion, deletion, transposition});
        }
        last_row[a[i - 1]] = i;
    }
    const std::size_t result = d[at(n + 1, m + 1)];
    if (limit != kNpos && result > limit) return limit + 1;
    return result;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/// Джаро по кодпоинтам.
[[nodiscard]] double jaro_cp(const std::vector<std::uint32_t>& a,
                             const std::vector<std::uint32_t>& b) {
    if (a.empty() && b.empty()) return 1.0;
    if (a.empty() || b.empty()) return 0.0;
    if (a == b) return 1.0;

    const std::size_t match_window = std::max(a.size(), b.size()) / 2;
    const std::size_t window = match_window > 0 ? match_window - 1 : 0;

    std::vector<bool> a_matched(a.size(), false);
    std::vector<bool> b_matched(b.size(), false);
    std::size_t matches = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::size_t begin = i > window ? i - window : 0;
        const std::size_t end = std::min(i + window + 1, b.size());
        for (std::size_t j = begin; j < end; ++j) {
            if (b_matched[j] || a[i] != b[j]) continue;
            a_matched[i] = true;
            b_matched[j] = true;
            ++matches;
            break;
        }
    }
    if (matches == 0) return 0.0;

    double transpositions = 0.0;
    std::size_t k = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!a_matched[i]) continue;
        while (k < b.size() && !b_matched[k]) ++k;
        if (k < b.size() && a[i] != b[k]) transpositions += 1.0;
        ++k;
    }
    transpositions /= 2.0;

    const double m = static_cast<double>(matches);
    return (m / static_cast<double>(a.size()) + m / static_cast<double>(b.size()) +
            (m - transpositions) / m) / 3.0;
}

/// Длина наибольшей общей подпоследовательности по кодпоинтам.
[[nodiscard]] std::size_t lcs_cp(const std::vector<std::uint32_t>& a,
                                 const std::vector<std::uint32_t>& b) {
    if (a.empty() || b.empty()) return 0;
    const std::vector<std::uint32_t>* left = &a;
    const std::vector<std::uint32_t>* right = &b;
    if (left->size() > right->size()) std::swap(left, right);

    std::vector<std::size_t> previous(left->size() + 1, 0);
    std::vector<std::size_t> current(left->size() + 1, 0);
    for (std::size_t j = 1; j <= right->size(); ++j) {
        for (std::size_t i = 1; i <= left->size(); ++i) {
            if ((*left)[i - 1] == (*right)[j - 1]) current[i] = previous[i - 1] + 1;
            else current[i] = std::max(previous[i], current[i - 1]);
        }
        std::swap(previous, current);
        std::fill(current.begin(), current.end(), 0);
    }
    return previous[left->size()];
}

[[nodiscard]] bool needs_posix_quotes(char c) noexcept {
    static constexpr std::string_view kSpecial = "|&;<>()$`\\\"' \t\n*?[]#~=%!{}";
    return c == '\0' || kSpecial.find(c) != kNpos;
}

[[nodiscard]] bool needs_windows_quotes(char c) noexcept {
    static constexpr std::string_view kSpecial = " \t\n\v\"&|<>^()%!;,";
    return c == '\0' || kSpecial.find(c) != kNpos;
}

/// Спец-имена файлов Windows, которые нельзя использовать как имя.
[[nodiscard]] bool is_reserved_windows_name(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 22> kReserved = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    const std::string lowered = lower_ascii(name);
    for (auto reserved : kReserved) {
        if (lowered == reserved) return true;
    }
    return false;
}

}  // namespace

std::string escape_c(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '"':  out.append("\\\""); break;
            case '\'': out.append("\\'"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '\a': out.append("\\a"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\v': out.append("\\v"); break;
            case '\0': out.append("\\0"); break;
            default:
                if (is_printable_ascii(c)) {
                    out.push_back(c);
                } else {
                    out.push_back('\\');
                    out.append(hex_byte(static_cast<unsigned char>(c), false));
                }
                break;
        }
    }
    return out;
}

std::string unescape_c(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        const char next = s[++i];
        switch (next) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'a': out.push_back('\a'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '0': out.push_back('\0'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '\'': out.push_back('\''); break;
            case 'x': {
                if (i + 2 < s.size()) {
                    const int value = hex_pair_value(s[i + 1], s[i + 2]);
                    if (value >= 0) {
                        out.push_back(static_cast<char>(value));
                        i += 2;
                        break;
                    }
                }
                out.push_back('x');
                break;
            }
            case 'u': {
                const std::size_t consumed = decode_unicode_escape(s, i + 1, out);
                if (consumed == 0) {
                    out.push_back('u');
                } else {
                    i += consumed;
                }
                break;
            }
            default:
                out.push_back(next);
                break;
        }
    }
    return out;
}

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            default:
                if (byte < 0x20U) {
                    out.append("\\u00");
                    out.append(hex_byte(byte, false));
                } else {
                    out.push_back(c);   // UTF-8 проходит как есть: JSON это допускает
                }
                break;
        }
    }
    return out;
}

std::string unescape_json(std::string_view s) { return unescape_c(s); }

std::string escape_html(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out.append("&amp;"); break;
            case '<': out.append("&lt;"); break;
            case '>': out.append("&gt;"); break;
            case '"': out.append("&quot;"); break;
            case '\'': out.append("&#39;"); break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string unescape_html(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        bool matched = false;
        for (std::size_t k = 0; k < kHtmlNamed.size(); ++k) {
            const std::string_view entity = kHtmlNamed[k];
            if (s.substr(i, entity.size()) == entity) {
                out.append(kHtmlReplacement[k]);
                i += entity.size() - 1;
                matched = true;
                break;
            }
        }
        if (matched) continue;

        // Числовые сущности: &#1055; или &#x41F;
        const std::size_t semicolon = s.find(';', i);
        if (semicolon != kNpos && semicolon - i <= 10 && i + 2 < s.size() && s[i + 1] == '#') {
            const bool hex = s[i + 2] == 'x' || s[i + 2] == 'X';
            const std::string_view digits = s.substr(i + (hex ? 3 : 2),
                                                    semicolon - i - (hex ? 3 : 2));
            const auto value = hex ? parse_uint(digits, 16) : parse_uint(digits, 10);
            if (value.has_value() && *value <= kMaxCodepoint) {
                utf8_append(out, static_cast<std::uint32_t>(*value));
                i = semicolon;
                continue;
            }
        }
        out.push_back('&');
    }
    return out;
}

std::string url_encode(std::string_view s, bool space_as_plus) {
    static constexpr std::string_view kUnreserved =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";
    std::string out;
    out.reserve(s.size() * 3);
    for (char c : s) {
        if (kUnreserved.find(c) != kNpos) {
            out.push_back(c);
            continue;
        }
        if (c == ' ' && space_as_plus) {
            out.push_back('+');
            continue;
        }
        out.push_back('%');
        out.append(hex_byte(static_cast<unsigned char>(c), true));
    }
    return out;
}

std::optional<std::string> url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '+') {
            out.push_back(' ');
            continue;
        }
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= s.size()) return std::nullopt;
        const int value = hex_pair_value(s[i + 1], s[i + 2]);
        if (value < 0) return std::nullopt;
        out.push_back(static_cast<char>(value));
        i += 2;
    }
    return out;
}

std::string url_path_join(std::string_view base, std::string_view relative) {
    std::string out(base);
    while (!out.empty() && out.back() == '/') out.pop_back();
    std::string tail(relative);
    while (!tail.empty() && tail.front() == '/') tail.erase(tail.begin());
    out.push_back('/');
    out.append(tail);
    return out;
}

std::string base64_encode(std::string_view data) {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 2 < data.size()) {
        const auto b0 = static_cast<unsigned>(static_cast<unsigned char>(data[i]));
        const auto b1 = static_cast<unsigned>(static_cast<unsigned char>(data[i + 1]));
        const auto b2 = static_cast<unsigned>(static_cast<unsigned char>(data[i + 2]));
        const unsigned triple = (b0 << 16U) | (b1 << 8U) | b2;
        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
        out.push_back(kAlphabet[triple & 0x3FU]);
        i += 3;
    }
    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const auto b0 = static_cast<unsigned>(static_cast<unsigned char>(data[i]));
        const unsigned triple = b0 << 16U;
        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        out.append("==");
    } else if (rest == 2) {
        const auto b0 = static_cast<unsigned>(static_cast<unsigned char>(data[i]));
        const auto b1 = static_cast<unsigned>(static_cast<unsigned char>(data[i + 1]));
        const unsigned triple = (b0 << 16U) | (b1 << 8U);
        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
        out.push_back('=');
    }
    return out;
}

std::optional<std::string> base64_decode(std::string_view text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char c : text) {
        if (is_ascii_space(c)) continue;
        if (c == '=') break;                       // дальше только выравнивание
        if (c == '-' || c == '_') {                // поддержка base64url «на входе»
            cleaned.push_back(c == '-' ? '+' : '/');
            continue;
        }
        cleaned.push_back(c);
    }
    std::string out;
    out.reserve((cleaned.size() / 4) * 3 + 3);

    unsigned buffer = 0;
    int bits = 0;
    for (char c : cleaned) {
        int value = -1;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        if (value < 0) return std::nullopt;
        buffer = (buffer << 6U) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return out;
}

std::string base64url_encode(std::string_view data) {
    std::string out = base64_encode(data);
    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::optional<std::string> base64url_decode(std::string_view text) {
    std::string padded(text);
    while (padded.size() % 4 != 0) padded.push_back('=');
    return base64_decode(padded);
}

std::string hex_encode(std::string_view data, bool uppercase) {
    std::string out;
    out.reserve(data.size() * 2);
    for (char c : data) out.append(hex_byte(static_cast<unsigned char>(c), uppercase));
    return out;
}

std::optional<std::string> hex_decode(std::string_view text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (char c : text) {
        if (is_ascii_space(c)) continue;
        if (c == 'x' || c == 'X' || c == '\\') continue;   // «0x41», «\x41» — тоже допустимо
        cleaned.push_back(c);
    }
    if (cleaned.size() % 2 != 0) return std::nullopt;
    std::string out;
    out.reserve(cleaned.size() / 2);
    for (std::size_t i = 0; i < cleaned.size(); i += 2) {
        const int value = hex_pair_value(cleaned[i], cleaned[i + 1]);
        if (value < 0) return std::nullopt;
        out.push_back(static_cast<char>(value));
    }
    return out;
}

std::string binary_dump(std::string_view data, std::size_t max_bytes) {
    const std::size_t limit = std::min(data.size(), max_bytes);
    std::string out;
    out.reserve(limit * 5);
    for (std::size_t offset = 0; offset < limit; offset += 16) {
        out.append(hex_byte(static_cast<unsigned>((offset >> 8U) & 0xFFU), false));
        out.append(hex_byte(static_cast<unsigned>(offset & 0xFFU), false));
        out.append("  ");
        std::string ascii;
        for (std::size_t i = 0; i < 16; ++i) {
            if (offset + i < limit) {
                const auto byte = static_cast<unsigned char>(data[offset + i]);
                out.append(hex_byte(byte, false));
                out.push_back(' ');
                ascii.push_back(byte >= 0x20U && byte < 0x7FU ? static_cast<char>(byte) : '.');
            } else {
                out.append("   ");
            }
        }
        out.append(" |");
        out.append(ascii);
        out.append("|\n");
    }
    if (data.size() > limit) {
        out.append("… ещё ");
        out.append(std::to_string(data.size() - limit));
        out.append(" байт\n");
    }
    return out;
}

std::string csv_quote(std::string_view field, char delimiter, char quote) {
    const bool needs_quotes = field.find(delimiter) != kNpos || field.find(quote) != kNpos ||
                              field.find('\n') != kNpos || field.find('\r') != kNpos ||
                              (!field.empty() && (field.front() == ' ' || field.back() == ' '));
    if (!needs_quotes) return std::string(field);
    std::string out;
    out.reserve(field.size() + 2);
    out.push_back(quote);
    for (char c : field) {
        if (c == quote) out.push_back(quote);   // удвоение кавычки — стандарт CSV
        out.push_back(c);
    }
    out.push_back(quote);
    return out;
}

std::vector<std::string> csv_parse_line(std::string_view line, char delimiter) {
    std::vector<std::string> out;
    std::string current;
    bool in_quotes = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (in_quotes) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;
                } else {
                    in_quotes = false;
                }
            } else {
                current.push_back(c);
            }
            continue;
        }
        if (c == '"' && current.empty()) {
            in_quotes = true;
            continue;
        }
        if (c == delimiter) {
            out.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    out.push_back(current);
    return out;
}

std::string csv_build(const std::vector<std::vector<std::string>>& rows, char delimiter) {
    std::string out;
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i != 0) out.push_back(delimiter);
            out.append(csv_quote(row[i], delimiter));
        }
        out.push_back('\n');
    }
    return out;
}

std::string shell_quote_posix(std::string_view argument) {
    if (argument.empty()) return "''";
    bool simple = true;
    for (char c : argument) {
        if (needs_posix_quotes(c)) {
            simple = false;
            break;
        }
    }
    if (simple) return std::string(argument);

    std::string out;
    out.reserve(argument.size() + 2);
    out.push_back('\'');
    for (char c : argument) {
        if (c == '\'') {
            out.append("'\\''");   // закрыть, экранировать, снова открыть
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string shell_quote_windows(std::string_view argument) {
    if (argument.empty()) return "\"\"";
    bool simple = true;
    for (char c : argument) {
        if (needs_windows_quotes(c)) {
            simple = false;
            break;
        }
    }
    if (simple) return std::string(argument);

    // Правила cmd.exe: экранируем кавычки и служебные символы, оборачиваем в кавычки,
    // удваивая обратные слэши перед закрывающей кавычкой.
    std::string out;
    out.reserve(argument.size() + 2);
    out.push_back('"');
    std::size_t backslashes = 0;
    for (char c : argument) {
        if (c == '\\') {
            ++backslashes;
            out.push_back(c);
            continue;
        }
        if (c == '"') {
            out.append(backslashes + 1, '\\');
            out.push_back('"');
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes, '\\');
    out.push_back('"');
    return out;
}

std::string shell_join_posix(const std::vector<std::string>& arguments) {
    std::string out;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i != 0) out.push_back(' ');
        out.append(shell_quote_posix(arguments[i]));
    }
    return out;
}

std::vector<std::string> split_command_line(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (char c : text) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\' && !in_single) {
            escaped = true;
            continue;
        }
        if (in_single) {
            if (c == '\'') in_single = false;
            else current.push_back(c);
            continue;
        }
        if (in_double) {
            if (c == '"') in_double = false;
            else current.push_back(c);
            continue;
        }
        if (c == '\'') {
            in_single = true;
            continue;
        }
        if (c == '"') {
            in_double = true;
            continue;
        }
        if (is_ascii_space(c)) {
            if (!current.empty()) {
                out.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (escaped) current.push_back('\\');
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string sanitize_filename(std::string_view name, char replacement) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        const bool invalid = byte < 0x20U || c == '<' || c == '>' || c == ':' || c == '"' ||
                             c == '/' || c == '\\' || c == '|' || c == '?' || c == '*' ||
                             c == 0x7F;
        if (invalid) {
            if (out.empty() || out.back() != replacement) out.push_back(replacement);
            continue;
        }
        out.push_back(c);
    }
    // Windows не любит имена, оканчивающиеся точкой или пробелом.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    if (out.empty()) out = "file";
    if (is_reserved_windows_name(out)) out += "_";
    if (out.size() > 200) {
        const std::string ext = path_extension(out);
        const std::size_t keep = 200 > ext.size() ? 200 - ext.size() : 100;
        out = utf8_truncate(out, static_cast<int>(keep), "") + ext;
    }
    return out;
}

std::string slugify(std::string_view text, char separator) {
    const std::string latin = transliterate(text);
    std::string out;
    out.reserve(latin.size());
    bool pending_separator = false;
    for (char c : latin) {
        const char lowered = ascii_lower(c);
        if (is_ascii_alnum(lowered)) {
            if (pending_separator && !out.empty()) out.push_back(separator);
            pending_separator = false;
            out.push_back(lowered);
            continue;
        }
        if (c == ' ' || c == '-' || c == '_' || c == '.' || c == '/' || c == '\\') {
            pending_separator = true;
            continue;
        }
        // Остальные символы (в том числе китайские/эмодзи) — в ASCII не влезают.
        pending_separator = true;
    }
    while (!out.empty() && out.back() == separator) out.pop_back();
    return out;
}

std::string normalize_path(std::string_view path) {
    if (path.empty()) return {};

    std::string unified;
    unified.reserve(path.size());
    for (char c : path) unified.push_back(c == '\\' ? '/' : c);

    std::string prefix;
    std::size_t start = 0;
    if (unified.size() >= 2 && is_ascii_alpha(unified[0]) && unified[1] == ':') {   // C:
        prefix = unified.substr(0, 2);
        start = 2;
    }
    bool absolute = false;
    while (start < unified.size() && unified[start] == '/') {
        absolute = true;
        ++start;
    }
    if (!prefix.empty()) prefix.push_back('/');
    else if (absolute) prefix = "/";

    std::vector<std::string> parts;
    for (auto piece : split(std::string_view(unified).substr(start), '/', false)) {
        if (piece.empty() || piece == ".") continue;
        if (piece == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute && prefix.empty()) {
                parts.push_back("..");
            }
            continue;
        }
        parts.push_back(piece);
    }

    std::string out = prefix;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out.push_back('/');
        out.append(parts[i]);
    }
    if (out.empty()) out = absolute ? "/" : ".";
    return out;
}

bool path_is_absolute(std::string_view path) noexcept {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    return path.size() >= 2 && is_ascii_alpha(path[0]) && path[1] == ':';
}

std::string path_join(std::string_view a, std::string_view b) {
    if (b.empty()) return std::string(a);
    if (path_is_absolute(b)) return normalize_path(b);
    if (a.empty()) return normalize_path(b);
    std::string out(a);
    if (out.back() != '/' && out.back() != '\\') out.push_back('/');
    out.append(b);
    return normalize_path(out);
}

std::string path_filename(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return std::string(slash == kNpos ? path : path.substr(slash + 1));
}

std::string path_extension(std::string_view path) {
    const std::string name = path_filename(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == kNpos || dot == 0) return {};
    return name.substr(dot);
}

std::string path_stem(std::string_view path) {
    const std::string name = path_filename(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == kNpos || dot == 0) return name;
    return name.substr(0, dot);
}

std::string path_parent(std::string_view path) {
    const std::string normalized = normalize_path(path);
    const std::size_t slash = normalized.find_last_of('/');
    if (slash == kNpos) return ".";
    if (slash == 0) return "/";
    return normalized.substr(0, slash);
}

std::vector<std::string> path_parts(std::string_view path) {
    std::string normalized = normalize_path(path);
    const bool absolute = !normalized.empty() && normalized[0] == '/';
    std::vector<std::string> out;
    if (absolute) out.emplace_back("/");
    for (auto& piece : split(normalized, '/', false)) out.push_back(piece);
    return out;
}

std::string path_relative(std::string_view path, std::string_view base) {
    const std::string target = normalize_path(path);
    const std::string root = normalize_path(base);
    if (target == root) return ".";
    if (starts_with(target, root + "/")) return target.substr(root.size() + 1);
    return target;
}

bool path_is_inside(std::string_view path, std::string_view directory) {
    const std::string target = normalize_path(path);
    const std::string root = normalize_path(directory);
    if (root.empty()) return false;
    if (target == root) return true;
    if (starts_with(target, root + "/")) return true;
    return false;
}

// ===========================================================================
//  6. Хеши и случайность
// ===========================================================================

namespace {

[[nodiscard]] std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

constexpr std::array<std::uint32_t, 64> kSha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U,
};

constexpr std::array<std::uint32_t, 8> kSha256Initial = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU,
    0x5be0cd19U,
};

void sha256_process_block(const std::uint8_t* block, std::array<std::uint32_t, 8>& state) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^
                                 (w[i - 15] >> 3U);
        const std::uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^
                                 (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state[0];
    std::uint32_t b = state[1];
    std::uint32_t c = state[2];
    std::uint32_t d = state[3];
    std::uint32_t e = state[4];
    std::uint32_t f = state[5];
    std::uint32_t g = state[6];
    std::uint32_t h = state[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256Constants[i] + w[i];
        const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

}  // namespace

std::uint64_t fnv1a64(std::string_view data) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;  // 0xcbf29ce484222325 — база FNV-1a
    for (char c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint32_t crc32(std::string_view data) noexcept {
    // Таблица строится один раз потокобезопасно (инициализация статической локальной
    // переменной в C++11 гарантированно однократна).
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xEDB88320U ^ (value >> 1U)) : (value >> 1U);
            }
            result[i] = value;
        }
        return result;
    }();

    std::uint32_t crc = 0xFFFFFFFFU;
    for (char c : data) {
        crc = table[(crc ^ static_cast<unsigned char>(c)) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

std::array<std::uint8_t, 32> sha256(std::string_view data) noexcept {
    std::array<std::uint32_t, 8> state = kSha256Initial;

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::size_t total = data.size();
    std::size_t processed = 0;

    std::array<std::uint8_t, 64> block{};
    while (total - processed >= 64) {
        sha256_process_block(bytes + processed, state);
        processed += 64;
    }

    const std::size_t rest = total - processed;
    block.fill(0);
    for (std::size_t i = 0; i < rest; ++i) block[i] = bytes[processed + i];
    block[rest] = 0x80U;

    if (rest >= 56) {
        sha256_process_block(block.data(), state);
        block.fill(0);
    }
    const std::uint64_t bits = static_cast<std::uint64_t>(total) * 8ULL;
    for (std::size_t i = 0; i < 8; ++i) {
        block[63 - i] = static_cast<std::uint8_t>((bits >> (8U * i)) & 0xFFU);
    }
    sha256_process_block(block.data(), state);

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((state[i] >> 24U) & 0xFFU);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 16U) & 0xFFU);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xFFU);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i] & 0xFFU);
    }
    return digest;
}

std::string sha256_hex(std::string_view data) {
    const auto digest = sha256(data);
    std::string out;
    out.reserve(64);
    for (std::uint8_t byte : digest) out.append(hex_byte(byte, false));
    return out;
}

std::string short_hash(std::string_view data, std::size_t hex_chars) {
    const std::string full = sha256_hex(data);
    return full.substr(0, std::min(hex_chars, full.size()));
}

std::string random_token(std::size_t bytes, bool hex_only) {
    std::random_device device;
    std::mt19937_64 generator(
        (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device()));
    std::string raw;
    raw.reserve(bytes);
    for (std::size_t i = 0; i < bytes; ++i) {
        raw.push_back(static_cast<char>(generator() & 0xFFU));
    }
    return hex_only ? hex_encode(raw) : base64url_encode(raw);
}

std::string random_hex(std::size_t bytes) { return random_token(bytes, true); }

std::uint64_t stable_hash(std::string_view data) noexcept { return fnv1a64(data); }

// ===========================================================================
//  7. Сравнение, нечёткий поиск, wildcard, natural sort
// ===========================================================================

std::size_t levenshtein(std::string_view a, std::string_view b, std::size_t limit) {
    return levenshtein_cp(codepoints_of(a), codepoints_of(b), limit);
}

std::size_t damerau_levenshtein(std::string_view a, std::string_view b, std::size_t limit) {
    return damerau_cp(codepoints_of(a), codepoints_of(b), limit);
}

double jaro(std::string_view a, std::string_view b) {
    return jaro_cp(codepoints_of(a), codepoints_of(b));
}

double jaro_winkler(std::string_view a, std::string_view b, double prefix_scale) {
    const double base = jaro(a, b);
    if (base <= 0.0) return 0.0;
    std::size_t prefix = 0;
    const std::size_t limit = std::min<std::size_t>({a.size(), b.size(), 4});
    while (prefix < limit && a[prefix] == b[prefix]) ++prefix;
    return base + static_cast<double>(prefix) * prefix_scale * (1.0 - base);
}

std::size_t lcs_length(std::string_view a, std::string_view b) {
    return lcs_cp(codepoints_of(a), codepoints_of(b));
}

double similarity_ratio(std::string_view a, std::string_view b) {
    const auto left = codepoints_of(a);
    const auto right = codepoints_of(b);
    const std::size_t total = left.size() + right.size();
    if (total == 0) return 1.0;
    const std::size_t common = lcs_cp(left, right);
    return 2.0 * static_cast<double>(common) / static_cast<double>(total);
}

int fuzzy_score(std::string_view query, std::string_view candidate) {
    if (query.empty()) return 0;
    if (candidate.empty()) return 0;
    if (query == candidate) return 1000;

    const std::string query_lower = lower_ascii(query);
    const std::string candidate_lower = lower_ascii(candidate);
    const std::string query_view = query_lower;
    const std::string candidate_view = candidate_lower;

    const std::size_t position = candidate_view.find(query_view);
    if (position == 0) {
        // Совпадение с начала — самое ценное: чем меньше «хвост», тем лучше.
        return 900 - static_cast<int>(std::min<std::size_t>(candidate.size(), 200)) / 2;
    }
    if (position != kNpos) {
        return 700 - static_cast<int>(position) - static_cast<int>(candidate.size()) / 10;
    }

    // Совпадение по подпоследовательности (fuzzy): «cfg» найдёт «config».
    std::size_t qi = 0;
    int streak = 0;
    int best_streak = 0;
    int penalty = 0;
    std::size_t last_match = 0;
    bool first_match = true;
    for (std::size_t ci = 0; ci < candidate_view.size() && qi < query_view.size(); ++ci) {
        if (candidate_view[ci] != query_view[qi]) {
            ++penalty;
            streak = 0;
            continue;
        }
        if (!first_match) {
            const std::size_t gap = ci - last_match - 1;
            penalty += static_cast<int>(gap) * 2;   // разрывы дороже, чем просто лишние символы
        }
        first_match = false;
        last_match = ci;
        ++qi;
        ++streak;
        best_streak = std::max(best_streak, streak);
    }
    if (qi < query_view.size()) {
        // Не все символы нашлись — считаем по «похожести» и штрафуем сильно.
        const double ratio = similarity_ratio(query_lower, candidate_lower);
        return static_cast<int>(ratio * 200.0) - 60;
    }
    int score = 500 - penalty + best_streak * 10;
    // Короткие кандидаты предпочтительнее: «main.cpp» лучше «main_test_helpers.cpp».
    score -= static_cast<int>(candidate.size()) / 4;
    return score;
}

std::vector<std::size_t> fuzzy_positions(std::string_view query, std::string_view candidate) {
    std::vector<std::size_t> positions;
    if (query.empty()) return positions;
    const std::string query_lower = lower_ascii(query);
    const std::string candidate_lower = lower_ascii(candidate);

    // Сначала пробуем точное вхождение — тогда подсветка будет непрерывной.
    const std::size_t position = candidate_lower.find(query_lower);
    if (position != kNpos) {
        for (std::size_t i = 0; i < query.size(); ++i) positions.push_back(position + i);
        return positions;
    }
    std::size_t qi = 0;
    for (std::size_t ci = 0; ci < candidate_lower.size() && qi < query_lower.size(); ++ci) {
        if (candidate_lower[ci] == query_lower[qi]) {
            positions.push_back(ci);
            ++qi;
        }
    }
    return positions;
}

std::optional<std::string> best_match(std::string_view query, const std::vector<std::string>& candidates,
                                      int min_score) {
    int best = min_score - 1;
    std::optional<std::string> result;
    for (const auto& candidate : candidates) {
        const int score = fuzzy_score(query, candidate);
        if (score > best) {
            best = score;
            result = candidate;
        }
    }
    return result;
}

std::vector<std::pair<std::string, int>> rank_matches(std::string_view query,
                                                      const std::vector<std::string>& candidates,
                                                      std::size_t limit, int min_score) {
    std::vector<std::pair<std::string, int>> scored;
    scored.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        const int score = fuzzy_score(query, candidate);
        if (score >= min_score) scored.emplace_back(candidate, score);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const std::pair<std::string, int>& a, const std::pair<std::string, int>& b) {
                         if (a.second != b.second) return a.second > b.second;
                         return a.first < b.first;
                     });
    if (scored.size() > limit) scored.resize(limit);
    return scored;
}

int natural_compare(std::string_view a, std::string_view b) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        const bool a_digit = is_ascii_digit(a[i]);
        const bool b_digit = is_ascii_digit(b[j]);
        if (a_digit && b_digit) {
            // Сравниваем числа целиком: «file10» > «file9».
            std::size_t ai = i;
            std::size_t bj = j;
            while (ai < a.size() && is_ascii_digit(a[ai])) ++ai;
            while (bj < b.size() && is_ascii_digit(b[bj])) ++bj;
            const std::string_view an = a.substr(i, ai - i);
            const std::string_view bn = b.substr(j, bj - j);
            const std::string_view an_trim = std::string_view(an).substr(
                std::min(an.find_first_not_of('0'), an.size() - 1));
            const std::string_view bn_trim = std::string_view(bn).substr(
                std::min(bn.find_first_not_of('0'), bn.size() - 1));
            if (an_trim.size() != bn_trim.size()) return an_trim.size() < bn_trim.size() ? -1 : 1;
            if (an_trim != bn_trim) return an_trim < bn_trim ? -1 : 1;
            i = ai;
            j = bj;
            continue;
        }
        const char ca = ascii_lower(a[i]);
        const char cb = ascii_lower(b[j]);
        if (ca != cb) return ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i == a.size() && j == b.size()) return 0;
    return i == a.size() ? -1 : 1;
}

bool natural_less(std::string_view a, std::string_view b) { return natural_compare(a, b) < 0; }

bool wildcard_match(std::string_view pattern, std::string_view text, bool case_sensitive) noexcept {
    // Матчинг с мемоизацией по паре (позиция в шаблоне, позиция в тексте).
    // Почему так: у «звёздочки с откатом» (классический алгоритм) память только
    // про одну звёздочку, а агент постоянно работает с шаблонами вида «**/*.cpp»,
    // где нужны две; плюс мемоизация убирает вырождения на «*a*a*a*».
    const std::size_t pattern_length = pattern.size();
    const std::size_t text_length = text.size();
    const std::size_t row_size = text_length + 1;

    auto equal_char = [case_sensitive](char left, char right) noexcept {
        return case_sensitive ? left == right : ascii_lower(left) == ascii_lower(right);
    };
    auto is_separator = [](char c) noexcept { return c == '/' || c == '\\'; };

    // «[...]» — один элемент шаблона; возвращает true, если симвolah подходит,
    // и записывает в *after позицию сразу после ']'.
    auto class_matches = [&pattern, pattern_length, &equal_char](std::size_t p, char value,
                                                                 std::size_t* after) {
        std::size_t index = p + 1;
        bool negate = false;
        if (index < pattern_length && (pattern[index] == '!' || pattern[index] == '^')) {
            negate = true;
            ++index;
        }
        bool matched = false;
        bool closed = false;
        while (index < pattern_length) {
            if (pattern[index] == ']') {
                closed = true;
                break;
            }
            if (index + 2 < pattern_length && pattern[index + 1] == '-' && pattern[index + 2] != ']') {
                const char low = equal_char('a', 'A') ? pattern[index] : ascii_lower(pattern[index]);
                const char high = equal_char('a', 'A') ? pattern[index + 2]
                                                       : ascii_lower(pattern[index + 2]);
                const char current = equal_char('a', 'A') ? value : ascii_lower(value);
                if (current >= low && current <= high) matched = true;
                index += 3;
                continue;
            }
            if (equal_char(pattern[index], value)) matched = true;
            ++index;
        }
        if (!closed) {
            // Незакрытая «[» — обычный символ, а не набор.
            *after = p + 1;
            return equal_char('[', value);
        }
        *after = index + 1;
        return matched != negate;
    };

    // memo: -1 — «ещё не считали», 0 — «не совпадает», 1 — «совпадает».
    std::vector<signed char> memo((pattern_length + 1) * row_size, -1);
    std::function<bool(std::size_t, std::size_t)> match = [&](std::size_t p, std::size_t t) -> bool {
        signed char& slot = memo[p * row_size + t];
        if (slot >= 0) return slot != 0;

        bool result = false;
        if (p == pattern_length) {
            result = (t == text_length);
        } else {
            const char pc = pattern[p];
            if (pc == '*') {
                const bool double_star = (p + 1 < pattern_length && pattern[p + 1] == '*');
                const std::size_t next_p = double_star ? p + 2 : p + 1;
                result = match(next_p, t);                       // звёздочка «съела» ноль символов
                if (!result && t < text_length) {
                    if (double_star || !is_separator(text[t])) {
                        result = match(p, t + 1);                // и ещё один символ
                    }
                }
                // «**/» может не иметь ни одного каталога: пропускаем «**» и «/».
                if (!result && double_star && next_p < pattern_length &&
                    is_separator(pattern[next_p])) {
                    result = match(next_p + 1, t);
                }
            } else if (t < text_length) {
                if (pc == '?') {
                    result = !is_separator(text[t]) && match(p + 1, t + 1);
                } else if (pc == '[') {
                    std::size_t after = p + 1;
                    if (class_matches(p, text[t], &after)) result = match(after, t + 1);
                } else if (is_separator(pc)) {
                    result = is_separator(text[t]) && match(p + 1, t + 1);
                } else {
                    result = equal_char(pc, text[t]) && match(p + 1, t + 1);
                }
            }
        }
        slot = result ? 1 : 0;
        return result;
    };

    return match(0, 0);
}

std::string glob_to_regex(std::string_view pattern, bool case_sensitive) {
    std::string out = case_sensitive ? "\\A" : "(?i)\\A";
    out.reserve(pattern.size() * 2 + 8);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        switch (c) {
            case '*': {
                if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                    out.append(".*");     // «**» — любые каталоги
                    ++i;
                    if (i + 1 < pattern.size() && (pattern[i + 1] == '/' || pattern[i + 1] == '\\')) {
                        ++i;
                    }
                } else {
                    out.append("[^/\\\\]*");   // «*» — в пределах одного сегмента
                }
                break;
            }
            case '?': out.append("[^/\\\\]"); break;
            case '.': case '^': case '$': case '+': case '(': case ')':
            case '{': case '}': case '|': case '[': case ']': case '\\':
                out.push_back('\\');
                out.push_back(c);
                break;
            case '/': out.append("[/\\\\]"); break;   // поддерживаем оба разделителя пути
            default: out.push_back(c); break;
        }
    }
    out.append("\\z");
    return out;
}




// ===========================================================================
//  8. Текст, таблицы, разметка, статистика
//
//  Всё, что нужно, чтобы показать агенту и человеку понятный текст: перенос по
//  ширине с учётом CJK, таблицы, рамки, прогресс, разметка Markdown, статистика
//  файла и оценка числа токенов (для бюджета контекста).
// ===========================================================================

namespace {

/// Разбивает строку на «слова» с сохранением пробелов: нужно diff_words.
[[nodiscard]] std::vector<std::string> tokenize_for_diff(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    bool in_space = false;
    for (char c : text) {
        const bool space = is_ascii_space(c);
        if (current.empty()) {
            in_space = space;
        } else if (space != in_space) {
            out.push_back(current);
            current.clear();
            in_space = space;
        }
        current.push_back(c);
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

/// Слова-«шум» для выделения ключевых терминов (русский + английский).
[[nodiscard]] bool is_stop_word(std::string_view word) {
    static constexpr std::string_view kStop[] = {
        "и", "в", "во", "не", "что", "он", "на", "я", "с", "со", "как", "а", "то", "все", "она",
        "так", "его", "но", "да", "ты", "к", "у", "же", "вы", "за", "бы", "по", "только", "ее",
        "мне", "было", "вот", "от", "меня", "еще", "нет", "о", "из", "ему", "теперь", "когда",
        "даже", "ну", "вдруг", "ли", "если", "уже", "или", "ни", "быть", "был", "него", "до",
        "вас", "нибудь", "опять", "уж", "вам", "ведь", "там", "потом", "себя", "ничего", "ей",
        "может", "они", "тут", "где", "есть", "надо", "ней", "для", "мы", "тебя", "их", "чем",
        "была", "сам", "чтоб", "без", "будто", "чего", "раз", "тоже", "себе", "под", "будет",
        "ж", "тогда", "кто", "этот", "того", "потому", "этого", "какой", "совсем", "ним", "здесь",
        "этом", "один", "почти", "мой", "тем", "чтобы", "нее", "сейчас", "были", "куда", "зачем",
        "всех", "никогда", "можно", "при", "наконец", "два", "об", "другой", "хоть", "после",
        "над", "больше", "тот", "через", "эти", "нас", "про", "всего", "них", "какая", "много",
        "разве", "три", "эту", "моя", "впрочем", "хорошо", "свою", "этой", "перед", "иногда",
        "лучше", "чуть", "том", "нельзя", "такой", "им", "более", "всегда", "конечно", "всю",
        "между", "the", "and", "for", "are", "but", "not", "you", "all", "any", "can", "her",
        "was", "one", "our", "out", "day", "get", "has", "him", "his", "how", "its", "new",
        "now", "old", "see", "two", "way", "who", "boy", "did", "use", "with", "that", "this",
        "from", "they", "have", "were", "been", "will", "your", "when", "there", "what", "about",
        "which", "their", "would", "into", "than", "then", "them", "these", "such", "only",
        "other", "some", "could", "should", "because", "while",
    };
    const std::string lowered = lower_unicode(word);
    for (auto stop : kStop) {
        if (lowered == stop) return true;
    }
    return lowered.size() < 3;
}

}  // namespace

std::string wrap(std::string_view text, int width) {
    if (width <= 1) return std::string(text);
    const std::string collapsed = collapse_spaces(text);
    if (collapsed.empty()) return {};

    std::string out;
    out.reserve(collapsed.size() + collapsed.size() / 8);
    int line_width = 0;
    std::size_t offset = 0;

    while (offset < collapsed.size()) {
        // Выделяем очередное слово целиком (в байтах UTF-8).
        std::size_t word_end = offset;
        while (word_end < collapsed.size() && collapsed[word_end] != ' ') ++word_end;
        const std::string_view word(collapsed.data() + offset, word_end - offset);

        const int word_width = display_width(word);
        if (line_width == 0) {
            if (word_width <= width) {
                out.append(word);
                line_width = word_width;
            } else {
                // Слишком длинное слово (путь, URL) — режем по колонкам.
                std::string_view rest = word;
                while (display_width(rest) > width) {
                    const std::string_view chunk = utf8_slice_columns(rest, 0, width);
                    out.append(chunk);
                    out.push_back('\n');
                    rest = rest.substr(chunk.size());
                }
                out.append(rest);
                line_width = display_width(rest);
            }
        } else if (line_width + 1 + word_width <= width) {
            out.push_back(' ');
            out.append(word);
            line_width += 1 + word_width;
        } else {
            out.push_back('\n');
            out.append(word);
            line_width = word_width;
        }
        offset = word_end;
        while (offset < collapsed.size() && collapsed[offset] == ' ') ++offset;
    }
    return out;
}

std::string wrap_block(std::string_view text, int width) {
    std::string out;
    const auto lines = line_views(text, true);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty()) {
            out.push_back('\n');
            continue;
        }
        // Строки с отступом (код, списки) не переформатируем: ломать структуру нельзя.
        const std::string_view trimmed = trim_left(lines[i]);
        const std::size_t indent_size = static_cast<std::size_t>(trimmed.data() - lines[i].data());
        if (indent_size >= 4 || starts_with(trimmed, "```") || starts_with(trimmed, "|")) {
            out.append(lines[i]);
        } else {
            out.append(wrap(trimmed, width - static_cast<int>(indent_size)));
        }
        if (i + 1 < lines.size()) out.push_back('\n');
    }
    return out;
}

std::string clip(std::string_view s, int max_width, std::string_view ellipsis) {
    return utf8_truncate(s, max_width, ellipsis);
}

std::string clip_middle(std::string_view s, int max_width, std::string_view ellipsis) {
    if (display_width(s) <= max_width) return std::string(s);
    const int ellipsis_width = display_width(ellipsis);
    if (max_width <= ellipsis_width) return std::string(ellipsis);
    const int remaining = max_width - ellipsis_width;
    const int head_width = (remaining + 1) / 2;
    const int tail_width = remaining - head_width;

    std::string out(utf8_slice_columns(s, 0, head_width));
    out.append(ellipsis);
    const std::string stripped = strip_ansi(s);
    const int total = display_width(stripped);
    out.append(utf8_slice_columns(stripped, total - tail_width, tail_width));
    return out;
}

std::string clip_lines(std::string_view s, std::size_t max_lines, std::string_view ellipsis) {
    const auto lines = line_views(s, true);
    if (lines.size() <= max_lines) return std::string(s);
    std::string out;
    for (std::size_t i = 0; i < max_lines; ++i) {
        out.append(lines[i]);
        out.push_back('\n');
    }
    out.append(ellipsis);
    out.push_back('\n');
    out.append("… ещё ");
    out.append(std::to_string(lines.size() - max_lines));
    out.append(" строк");
    return out;
}

std::string numbered_lines(std::string_view s, int start, int number_width) {
    const auto lines = line_views(s, true);
    const int last = start + static_cast<int>(lines.size()) - 1;
    if (number_width <= 0) {
        number_width = static_cast<int>(std::to_string(last < 0 ? 0 : last).size());
    }
    std::string out;
    out.reserve(s.size() + lines.size() * 6);
    int number = start;
    for (auto line : lines) {
        out.append(pad_left(std::to_string(number), number_width));
        out.append(" │ ");
        out.append(line);
        out.push_back('\n');
        ++number;
    }
    return out;
}

std::string table(const std::vector<std::vector<std::string>>& rows,
                  const std::vector<std::string>& headers, int max_width) {
    std::size_t columns = headers.size();
    for (const auto& row : rows) columns = std::max(columns, row.size());
    if (columns == 0) return {};

    std::vector<int> widths(columns, 1);
    auto measure = [&](const std::vector<std::string>& row) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            widths[i] = std::max(widths[i], terminal_width(row[i]));
        }
    };
    measure(headers);
    for (const auto& row : rows) measure(row);

    // При заданной максимальной ширине сжимаем самые широкие столбцы.
    if (max_width > 0) {
        const int overhead = static_cast<int>(columns) * 3 + 1;
        auto total = [&] {
            int sum = overhead;
            for (int w : widths) sum += w;
            return sum;
        };
        while (total() > max_width) {
            std::size_t widest = 0;
            for (std::size_t i = 1; i < widths.size(); ++i) {
                if (widths[i] > widths[widest]) widest = i;
            }
            if (widths[widest] <= 4) break;
            --widths[widest];
        }
    }

    auto border = [&](std::string_view left, std::string_view middle, std::string_view right) {
        std::string out(left);
        for (std::size_t i = 0; i < columns; ++i) {
            out.append(repeat("─", static_cast<std::size_t>(widths[i]) + 2));
            out.append(i + 1 == columns ? right : middle);
        }
        return out;
    };
    auto render_row = [&](const std::vector<std::string>& row, bool header_row) {
        std::string out("│");
        for (std::size_t i = 0; i < columns; ++i) {
            const std::string_view cell = i < row.size() ? std::string_view(row[i]) : std::string_view{};
            std::string cell_text = truncate_visible(cell, widths[i]);
            std::string padded = cell_text + std::string(
                static_cast<std::size_t>(std::max(0, widths[i] - terminal_width(cell_text))), ' ');
            out.push_back(' ');
            out.append(header_row ? bold_text(padded) : padded);
            out.append(" │");
        }
        return out;
    };

    std::string out;
    out.append(border("┌", "┬", "┐"));
    out.push_back('\n');
    if (!headers.empty()) {
        out.append(render_row(headers, true));
        out.push_back('\n');
        out.append(border("├", "┼", "┤"));
        out.push_back('\n');
    }
    for (const auto& row : rows) {
        out.append(render_row(row, false));
        out.push_back('\n');
    }
    out.append(border("└", "┴", "┘"));
    return out;
}

std::string box(std::string_view title, std::string_view body, int width) {
    const auto body_lines = line_views(body, true);
    int inner = display_width(title) + 2;
    for (auto line : body_lines) inner = std::max(inner, terminal_width(line));
    if (width > 0) inner = std::max(4, width - 2);

    std::string out;
    const auto title_width = static_cast<std::size_t>(display_width(title));
    if (!title.empty()) {
        out.append("┌─ ");
        out.append(title);
        out.push_back(' ');
        const std::size_t fill = static_cast<std::size_t>(inner) > title_width + 2
                                     ? static_cast<std::size_t>(inner) - title_width - 2
                                     : 0;
        out.append(repeat("─", fill));
        out.append("┐");
    } else {
        out.append("┌");
        out.append(repeat("─", static_cast<std::size_t>(inner)));
        out.append("┐");
    }
    out.push_back('\n');

    for (auto line : body_lines) {
        std::string text(truncate_visible(line, inner));
        out.append("│ ");
        out.append(text);
        out.append(std::string(static_cast<std::size_t>(
                                   std::max(0, inner - terminal_width(text) - 1)), ' '));
        out.append("│\n");
    }
    out.append("└");
    out.append(repeat("─", static_cast<std::size_t>(inner)));
    out.append("┘");
    return out;
}

std::string progress_bar(double fraction, int width, bool with_percent) {
    if (width < 4) width = 4;
    fraction = std::min(std::max(fraction, 0.0), 1.0);
    const int filled = static_cast<int>(fraction * static_cast<double>(width) + 0.5);
    std::string out = "[";
    out.append(repeat("█", static_cast<std::size_t>(filled)));
    out.append(repeat("░", static_cast<std::size_t>(width - filled)));
    out.push_back(']');
    if (with_percent) {
        out.push_back(' ');
        out.append(pad_left(format_percent(fraction, 0), 4));
    }
    return out;
}

std::string spinner_frame(std::size_t tick) {
    static constexpr std::array<std::string_view, 10> kFrames = {
        "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏",
    };
    return std::string(kFrames[tick % kFrames.size()]);
}

std::string markdown_to_ansi(std::string_view markdown) {
    std::string out;
    out.reserve(markdown.size() * 2);
    bool in_code_block = false;
    const auto lines = line_views(markdown, true);

    for (std::size_t index = 0; index < lines.size(); ++index) {
        std::string_view line = lines[index];

        if (starts_with(trim_left(line), "```")) {
            in_code_block = !in_code_block;
            out.append(gray_text(trim_left(line)));
            out.push_back('\n');
            continue;
        }
        if (in_code_block) {
            out.append(gray_text(line));
            out.push_back('\n');
            continue;
        }

        // Заголовки: чем больше решёток, тем тусклее (но всё равно жирнее обычного).
        std::size_t hashes = 0;
        while (hashes < line.size() && line[hashes] == '#') ++hashes;
        if (hashes >= 1 && hashes <= 6 && hashes + 1 < line.size() && line[hashes] == ' ') {
            const std::string_view heading = trim(line.substr(hashes + 1));
            out.append(bold_text(hashes <= 2 ? std::string(heading) : cyan_text(heading)));
            out.push_back('\n');
            continue;
        }

        if (starts_with(line, ">")) {
            out.append(colorize(trim_left(line.substr(1)), ansi::gray));
            out.push_back('\n');
            continue;
        }
        const std::string_view trimmed_line = trim(line);
        if (trimmed_line == "---" || trimmed_line == "***" || trimmed_line == "___") {
            out.append(gray_text(repeat("─", 40)));
            out.push_back('\n');
            continue;
        }
        if (starts_with(trimmed_line, "- ") || starts_with(trimmed_line, "* ") ||
            starts_with(trimmed_line, "+ ")) {
            out.append("  • ");
            std::string rest(trimmed_line.substr(2));
            rest = replace_all(rest, "**", "");
            out.append(rest);
            out.push_back('\n');
            continue;
        }

        // Внутри строки: `код`, **жирный**, *курсив*, [текст](ссылка).
        std::string rendered;
        bool in_inline_code = false;
        bool in_bold = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '`') {
                in_inline_code = !in_inline_code;
                rendered.append(in_inline_code ? "\x1b[36m" : "\x1b[0m");
                continue;
            }
            if (!in_inline_code && i + 1 < line.size() && line[i] == '*' && line[i + 1] == '*') {
                in_bold = !in_bold;
                rendered.append(in_bold ? "\x1b[1m" : "\x1b[0m");
                ++i;
                continue;
            }
            if (!in_inline_code && line[i] == '[') {
                const std::size_t close = line.find(']', i);
                if (close != kNpos && close + 1 < line.size() && line[close + 1] == '(') {
                    const std::size_t end = line.find(')', close + 2);
                    if (end != kNpos) {
                        const std::string_view label = line.substr(i + 1, close - i - 1);
                        const std::string_view url = line.substr(close + 2, end - close - 2);
                        rendered.append(cyan_text(label));
                        rendered.append(gray_text(" <" + std::string(url) + ">"));
                        i = end;
                        continue;
                    }
                }
            }
            rendered.push_back(line[i]);
        }
        rendered.append("\x1b[0m");
        out.append(rendered);
        out.push_back('\n');
    }
    return out;
}

std::string strip_markdown(std::string_view markdown) {
    std::string text = markdown_to_ansi(markdown);
    text = strip_ansi(text);
    // Убираем остатки служебной разметки, которую не тронул рендер.
    for (std::string_view marker : {"```", "**", "|"}) {
        text = replace_all(text, marker, "");
    }
    return text;
}

std::string highlight(std::string_view text, const std::vector<std::string>& terms,
                      std::string_view code) {
    if (terms.empty()) return std::string(text);
    std::string out;
    out.reserve(text.size() + 32);
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::size_t best_length = 0;
        for (const auto& term : terms) {
            if (term.empty()) continue;
            if (starts_with(std::string_view(text).substr(offset), term) ||
                istarts_with_ascii(text.substr(offset), term)) {
                best_length = std::max(best_length, term.size());
            }
        }
        if (best_length > 0) {
            out.append(colorize(text.substr(offset, best_length), code));
            offset += best_length;
            continue;
        }
        out.push_back(text[offset]);
        ++offset;
    }
    return out;
}

std::string render_template(std::string_view tmpl, const std::map<std::string, std::string>& vars) {
    std::string out;
    out.reserve(tmpl.size());
    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{' && i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
            const std::size_t close = tmpl.find("}}", i + 2);
            if (close != kNpos) {
                const std::string key(trim(tmpl.substr(i + 2, close - i - 2)));
                const auto found = vars.find(key);
                if (found != vars.end()) out.append(found->second);
                else out.append("{{").append(key).append("}}");
                i = close + 2;
                continue;
            }
        }
        out.push_back(tmpl[i]);
        ++i;
    }
    return out;
}

std::vector<std::string> template_variables(std::string_view tmpl) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{' && i + 1 < tmpl.size() && tmpl[i + 1] == '{') {
            const std::size_t close = tmpl.find("}}", i + 2);
            if (close == kNpos) break;
            const std::string key(trim(tmpl.substr(i + 2, close - i - 2)));
            if (!key.empty() && std::find(out.begin(), out.end(), key) == out.end()) {
                out.push_back(key);
            }
            i = close + 2;
            continue;
        }
        ++i;
    }
    return out;
}

TextStats text_stats(std::string_view text) {
    TextStats stats;
    stats.bytes = text.size();
    stats.codepoints = utf8_length(text);
    stats.graphemes = grapheme_count(text);

    const auto lines = line_views(text, true);
    stats.lines = lines.size();
    double width_sum = 0.0;
    for (auto line : lines) {
        const int width = terminal_width(line);
        width_sum += static_cast<double>(width);
        stats.max_line_width = std::max<std::size_t>(stats.max_line_width,
                                                     static_cast<std::size_t>(width));
        if (width > 120) ++stats.long_lines;
        if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) stats.has_trailing_space = true;
        if (line.find('\t') != kNpos) stats.has_tabs = true;
    }
    stats.average_line_width = stats.lines == 0 ? 0.0 : width_sum / static_cast<double>(stats.lines);
    stats.has_crlf = text.find("\r\n") != kNpos;
    stats.words = words(text).size();
    stats.sentences = sentences(text).size();
    return stats;
}

std::string text_stats_report(std::string_view text) {
    const TextStats stats = text_stats(text);
    std::vector<std::vector<std::string>> rows = {
        {"Строк", std::to_string(stats.lines)},
        {"Слов", std::to_string(stats.words)},
        {"Предложений", std::to_string(stats.sentences)},
        {"Символов (кодпоинтов)", std::to_string(stats.codepoints)},
        {"Байт", std::to_string(stats.bytes)},
        {"Оценка токенов", std::to_string(estimate_tokens(text))},
        {"Длинных строк (>120)", std::to_string(stats.long_lines)},
        {"Максимальная ширина", std::to_string(stats.max_line_width)},
        {"Средняя ширина", format_double(stats.average_line_width, 1)},
        {"Перевод строк CRLF", stats.has_crlf ? "да" : "нет"},
        {"Табуляции", stats.has_tabs ? "да" : "нет"},
        {"Пробелы в конце строк", stats.has_trailing_space ? "да" : "нет"},
    };
    return table(rows, {"Показатель", "Значение"});
}

std::size_t estimate_tokens(std::string_view text) {
    // Практическая эвристика: латиница/код — примерно 4 символа на токен,
    // кириллица и CJK «дороже» (1–2 символа на токен). Считаем по байтам UTF-8:
    // ASCII-байты делим на 4, многобайтовые считаем по 1.6 байта на токен.
    std::size_t ascii_bytes = 0;
    std::size_t multibyte_bytes = 0;
    for (char c : text) {
        if (static_cast<unsigned char>(c) < 0x80U) ++ascii_bytes;
        else ++multibyte_bytes;
    }
    const double tokens = static_cast<double>(ascii_bytes) / 4.0 +
                          static_cast<double>(multibyte_bytes) / 1.6;
    return static_cast<std::size_t>(tokens + 0.5);
}

std::vector<std::string> sentences(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    const auto codepoints = utf8_decode(text);
    for (std::size_t i = 0; i < codepoints.size(); ++i) {
        const std::uint32_t cp = codepoints[i];
        current.append(utf8_encode(cp));
        const bool terminator = cp == '.' || cp == '!' || cp == '?' || cp == 0x2026U ||
                                cp == 0x3002U || cp == '\n';
        if (!terminator) continue;
        const std::string trimmed = trim(current).empty() ? std::string{}
                                                          : std::string(trim(current));
        if (trimmed.size() >= 2) out.push_back(trimmed);
        current.clear();
    }
    const std::string tail(trim(current));
    if (tail.size() >= 2) out.push_back(tail);
    return out;
}

std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    for (std::uint32_t cp : utf8_decode(text)) {
        if (is_word_cp(cp)) {
            utf8_append(current, cp);
            continue;
        }
        if (!current.empty()) {
            out.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::vector<std::pair<std::string, int>> word_frequencies(std::string_view text) {
    std::map<std::string, int> counts;
    for (const auto& word : words(text)) {
        counts[lower_unicode(word)] += 1;
    }
    std::vector<std::pair<std::string, int>> out(counts.begin(), counts.end());
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    return out;
}

std::vector<std::pair<std::string, int>> key_terms(std::string_view text, std::size_t limit) {
    // Ищем не только отдельные слова, но и «двухсловные» термины: «vector index»,
    // «векторный индекс» — они куда полезнее для понимания о чём текст.
    std::map<std::string, int> counts;
    const auto tokens = words(text);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string lowered = lower_unicode(tokens[i]);
        if (!is_stop_word(lowered) && lowered.size() >= 3) counts[lowered] += 2;
        if (i + 1 < tokens.size()) {
            const std::string second = lower_unicode(tokens[i + 1]);
            if (!is_stop_word(lowered) && !is_stop_word(second) && lowered.size() >= 3 &&
                second.size() >= 3) {
                counts[lowered + " " + second] += 1;
            }
        }
    }
    std::vector<std::pair<std::string, int>> out(counts.begin(), counts.end());
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first.size() > b.first.size();
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

std::string keyword_summary(std::string_view text, std::size_t keywords, std::size_t sentences_limit) {
    const auto terms = key_terms(text, keywords);
    const auto all_sentences = sentences(text);
    if (all_sentences.empty()) return {};

    std::vector<std::pair<double, std::string>> scored;
    scored.reserve(all_sentences.size());
    for (const auto& sentence : all_sentences) {
        const std::string lowered = lower_unicode(sentence);
        double score = 0.0;
        for (const auto& term : terms) {
            if (lowered.find(term.first) != kNpos) score += static_cast<double>(term.second);
        }
        // Первые предложения обычно важнее (в файлах — заголовок и описание).
        score += 2.0;
        score -= static_cast<double>(sentence.size()) / 400.0;
        scored.emplace_back(score, sentence);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    if (scored.size() > sentences_limit) scored.resize(sentences_limit);

    std::string out;
    out.append("Ключевые термины: ");
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (i != 0) out.append(", ");
        out.append(terms[i].first);
    }
    out.append("\n\n");
    std::vector<std::string> chosen;
    chosen.reserve(scored.size());
    for (const auto& item : scored) chosen.push_back(item.second);
    // Возвращаем в исходном порядке, чтобы сводка читалась связно.
    std::vector<std::string> ordered;
    for (const auto& sentence : all_sentences) {
        if (std::find(chosen.begin(), chosen.end(), sentence) != chosen.end()) {
            ordered.push_back(sentence);
        }
    }
    for (const auto& sentence : ordered) {
        out.append("• ");
        out.append(collapse_spaces(sentence));
        out.push_back('\n');
    }
    return out;
}

bool looks_like_code(std::string_view text) noexcept {
    if (text.empty()) return false;
    std::size_t code_marks = 0;
    std::size_t lines = 0;
    for (char c : text) {
        if (c == ';' || c == '{' || c == '}' || c == '(' || c == ')' || c == '=' ||
            c == '#' || c == '<' || c == '>') {
            ++code_marks;
        }
        if (c == '\n') ++lines;
    }
    ++lines;
    static constexpr std::array<std::string_view, 12> kKeywords = {
        "def ", "class ", "void ", "int ", "return", "#include", "function ", "const ",
        "import ", "public ", "static ", "let ",
    };
    std::size_t keyword_hits = 0;
    for (auto keyword : kKeywords) {
        if (text.find(keyword) != kNpos) ++keyword_hits;
    }
    const double marks_per_line = static_cast<double>(code_marks) / static_cast<double>(lines);
    return keyword_hits >= 2 || marks_per_line > 3.0;
}

std::string detect_language_hint(std::string_view path) {
    const std::string extension = lower_ascii(path_extension(path));
    if (extension.empty()) return "text";
    static const std::map<std::string, std::string> kMap = {
        {".cpp", "cpp"}, {".cc", "cpp"}, {".cxx", "cpp"}, {".hpp", "cpp"}, {".h", "cpp"},
        {".c", "c"}, {".py", "python"}, {".pyi", "python"}, {".js", "javascript"},
        {".mjs", "javascript"}, {".ts", "typescript"}, {".tsx", "typescript"}, {".jsx", "javascript"},
        {".cs", "csharp"}, {".java", "java"}, {".kt", "kotlin"}, {".go", "go"}, {".rs", "rust"},
        {".rb", "ruby"}, {".php", "php"}, {".swift", "swift"}, {".m", "objective-c"},
        {".sh", "bash"}, {".ps1", "powershell"}, {".bat", "batch"}, {".cmd", "batch"},
        {".json", "json"}, {".yaml", "yaml"}, {".yml", "yaml"}, {".toml", "toml"},
        {".ini", "ini"}, {".cfg", "ini"}, {".xml", "xml"}, {".html", "html"}, {".htm", "html"},
        {".css", "css"}, {".scss", "scss"}, {".md", "markdown"}, {".rst", "rst"},
        {".sql", "sql"}, {".cmake", "cmake"}, {".lua", "lua"}, {".dart", "dart"},
        {".vue", "vue"}, {".svelte", "svelte"}, {".r", "r"}, {".pl", "perl"}, {".ex", "elixir"},
        {".txt", "text"}, {".log", "text"}, {".csv", "csv"}, {".tsv", "csv"},
    };
    const auto found = kMap.find(extension);
    if (found != kMap.end()) return found->second;
    return "text";
}

// ===========================================================================
//  9. Diff: сравнение текстов и патчи
//
//  Используем алгоритм Хиршберга (LCS за O(n·m) времени и O(min(n,m)) памяти):
//  он даёт минимальное редактирование и, в отличие от наивной матрицы, не съедает
//  гигабайты на больших файлах. Для очень больших входов включается предохранитель
//  (полная замена), чтобы агент не «завис» на сравнении двух логов.
// ===========================================================================

namespace {

/// Заполняет строку длин LCS для a[a_lo..a_hi) и b[b_lo..b_hi) (последняя строка DP).
[[nodiscard]] std::vector<std::size_t> lcs_row(const std::vector<std::string>& a, std::size_t a_lo,
                                               std::size_t a_hi,
                                               const std::vector<std::string>& b, std::size_t b_lo,
                                               std::size_t b_hi) {
    const std::size_t width = b_hi - b_lo;
    std::vector<std::size_t> previous(width + 1, 0);
    std::vector<std::size_t> current(width + 1, 0);
    for (std::size_t i = a_lo; i < a_hi; ++i) {
        current[0] = 0;
        for (std::size_t j = 0; j < width; ++j) {
            if (a[i] == b[b_lo + j]) current[j + 1] = previous[j] + 1;
            else current[j + 1] = std::max(previous[j + 1], current[j]);
        }
        std::swap(previous, current);
    }
    return previous;
}

void push_op(std::vector<DiffOp>& ops, DiffType type, const std::string& text) {
    if (!ops.empty() && ops.back().type == type) {
        ops.back().text += "\n";
        ops.back().text += text;
        return;
    }
    ops.push_back(DiffOp{type, text});
}

void hirschberg(const std::vector<std::string>& a, std::size_t a_lo, std::size_t a_hi,
                const std::vector<std::string>& b, std::size_t b_lo, std::size_t b_hi,
                std::vector<DiffOp>& ops, std::size_t budget) {
    if (a_lo >= a_hi) {
        for (std::size_t j = b_lo; j < b_hi; ++j) push_op(ops, DiffType::Insert, b[j]);
        return;
    }
    if (b_lo >= b_hi) {
        for (std::size_t i = a_lo; i < a_hi; ++i) push_op(ops, DiffType::Delete, a[i]);
        return;
    }
    if ((a_hi - a_lo) * (b_hi - b_lo) > budget) {
        for (std::size_t i = a_lo; i < a_hi; ++i) push_op(ops, DiffType::Delete, a[i]);
        for (std::size_t j = b_lo; j < b_hi; ++j) push_op(ops, DiffType::Insert, b[j]);
        return;
    }
    if (a_hi - a_lo == 1) {
        // Один элемент: ищем его в b — самый частый случай для маленьких файлов.
        for (std::size_t j = b_lo; j < b_hi; ++j) {
            if (a[a_lo] == b[j]) {
                for (std::size_t k = b_lo; k < j; ++k) push_op(ops, DiffType::Insert, b[k]);
                push_op(ops, DiffType::Equal, a[a_lo]);
                for (std::size_t k = j + 1; k < b_hi; ++k) push_op(ops, DiffType::Insert, b[k]);
                return;
            }
        }
        push_op(ops, DiffType::Delete, a[a_lo]);
        for (std::size_t j = b_lo; j < b_hi; ++j) push_op(ops, DiffType::Insert, b[j]);
        return;
    }

    const std::size_t a_mid = a_lo + (a_hi - a_lo) / 2;
    const std::vector<std::size_t> forward = lcs_row(a, a_lo, a_mid, b, b_lo, b_hi);

    // Обратный проход: считаем LCS для «хвоста» a и «хвоста» b.
    std::vector<std::string> a_reversed(a.rbegin() + static_cast<std::ptrdiff_t>(a.size() - a_hi),
                                        a.rbegin() + static_cast<std::ptrdiff_t>(a.size() - a_mid));
    std::vector<std::string> b_reversed(b.rbegin() + static_cast<std::ptrdiff_t>(b.size() - b_hi),
                                        b.rbegin() + static_cast<std::ptrdiff_t>(b.size() - b_lo));
    const std::vector<std::size_t> backward =
        lcs_row(a_reversed, 0, a_reversed.size(), b_reversed, 0, b_reversed.size());

    const std::size_t width = b_hi - b_lo;
    std::size_t split = 0;
    std::size_t best = 0;
    for (std::size_t k = 0; k <= width; ++k) {
        const std::size_t score = forward[k] + backward[width - k];
        if (score >= best) {
            best = score;
            split = k;
        }
    }
    hirschberg(a, a_lo, a_mid, b, b_lo, b_lo + split, ops, budget);
    hirschberg(a, a_mid, a_hi, b, b_lo + split, b_hi, ops, budget);
}

/// Общий вход: любые «элементы» сравниваются как строки.
[[nodiscard]] std::vector<DiffOp> diff_items(const std::vector<std::string>& a,
                                             const std::vector<std::string>& b) {
    std::vector<DiffOp> ops;
    if (a.empty() && b.empty()) return ops;
    // Снимаем общее начало и конец — это самое дешёвое ускорение.
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;

    std::size_t suffix = 0;
    while (suffix < (a.size() - prefix) && suffix < (b.size() - prefix) &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }

    for (std::size_t i = 0; i < prefix; ++i) push_op(ops, DiffType::Equal, a[i]);
    const std::vector<std::string> a_mid(a.begin() + static_cast<std::ptrdiff_t>(prefix),
                                         a.end() - static_cast<std::ptrdiff_t>(suffix));
    const std::vector<std::string> b_mid(b.begin() + static_cast<std::ptrdiff_t>(prefix),
                                         b.end() - static_cast<std::ptrdiff_t>(suffix));
    constexpr std::size_t kBudget = 40'000'000;   // предохранитель: ~40 млн сравнений
    hirschberg(a_mid, 0, a_mid.size(), b_mid, 0, b_mid.size(), ops, kBudget);
    for (std::size_t i = a.size() - suffix; i < a.size(); ++i) push_op(ops, DiffType::Equal, a[i]);
    return ops;
}

/// Текст → список строк. Завершающий перевод строки НЕ создаёт лишнюю пустую
/// строку: иначе diff и патчи добавляли бы пустую строку в конце файла.
[[nodiscard]] std::vector<std::string> to_line_vector(std::string_view text) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;
    for (auto line : line_views(text, true)) lines.emplace_back(line);
    if (!lines.empty() && lines.back().empty() && text.back() == '\n') lines.pop_back();
    return lines;
}

/// Разбор строки «@@ -12,3 +14,5 @@» в диапазоны.
struct HunkHeader {
    std::size_t a_start = 0;
    std::size_t a_count = 0;
    std::size_t b_start = 0;
    std::size_t b_count = 0;
    bool valid = false;
};

[[nodiscard]] HunkHeader parse_hunk_header(std::string_view line) {
    HunkHeader header;
    if (!starts_with(line, "@@")) return header;
    const std::size_t minus = line.find('-');
    const std::size_t plus = line.find('+');
    if (minus == kNpos || plus == kNpos) return header;

    auto read_range = [](std::string_view text, std::size_t* start, std::size_t* count) {
        const std::size_t comma = text.find(',');
        const std::string_view first = comma == kNpos ? text : text.substr(0, comma);
        const auto parsed_start = parse_uint(first, 10);
        if (!parsed_start.has_value()) return false;
        *start = static_cast<std::size_t>(*parsed_start);
        *count = 1;
        if (comma != kNpos) {
            const std::size_t end = text.find(' ', comma);
            const std::string_view second =
                end == kNpos ? text.substr(comma + 1) : text.substr(comma + 1, end - comma - 1);
            const auto parsed_count = parse_uint(second, 10);
            if (!parsed_count.has_value()) return false;
            *count = static_cast<std::size_t>(*parsed_count);
        }
        return true;
    };

    const std::size_t a_end = line.find(' ', minus);
    const std::string_view a_part =
        a_end == kNpos ? line.substr(minus + 1) : line.substr(minus + 1, a_end - minus - 1);
    const std::size_t b_end = line.find(' ', plus);
    const std::string_view b_part =
        b_end == kNpos ? line.substr(plus + 1) : line.substr(plus + 1, b_end - plus - 1);

    header.valid = read_range(a_part, &header.a_start, &header.a_count) &&
                   read_range(b_part, &header.b_start, &header.b_count);
    return header;
}

}  // namespace

std::string_view diff_type_name(DiffType type) noexcept {
    switch (type) {
        case DiffType::Equal: return "равно";
        case DiffType::Insert: return "добавлено";
        case DiffType::Delete: return "удалено";
    }
    return "неизвестно";
}

char diff_type_marker(DiffType type) noexcept {
    switch (type) {
        case DiffType::Equal: return ' ';
        case DiffType::Insert: return '+';
        case DiffType::Delete: return '-';
    }
    return '?';
}

std::string DiffStats::summary() const {
    std::string out = "+";
    out += std::to_string(added);
    out += " −";
    out += std::to_string(removed);
    if (identical()) out += " (без изменений)";
    return out;
}

std::vector<DiffOp> diff_lines(std::string_view a, std::string_view b) {
    return diff_items(to_line_vector(a), to_line_vector(b));
}

std::vector<DiffOp> diff_chars(std::string_view a, std::string_view b) {
    std::vector<std::string> left;
    std::vector<std::string> right;
    for (std::uint32_t cp : utf8_decode(a)) left.push_back(utf8_encode(cp));
    for (std::uint32_t cp : utf8_decode(b)) right.push_back(utf8_encode(cp));
    return diff_items(left, right);
}

std::vector<DiffOp> diff_words(std::string_view a, std::string_view b) {
    return diff_items(tokenize_for_diff(a), tokenize_for_diff(b));
}

DiffStats diff_stats(const std::vector<DiffOp>& ops) {
    DiffStats stats;
    for (const auto& op : ops) {
        if (op.text.empty() && op.type != DiffType::Equal) continue;
        const std::size_t lines = op.text.empty()
                                      ? 0
                                      : static_cast<std::size_t>(std::count(op.text.begin(),
                                                                            op.text.end(), '\n')) +
                                            1;
        switch (op.type) {
            case DiffType::Insert: stats.added += lines; break;
            case DiffType::Delete: stats.removed += lines; break;
            case DiffType::Equal: stats.unchanged += lines; break;
        }
    }
    return stats;
}

std::string unified_diff(std::string_view a, std::string_view b, std::string_view path_a,
                         std::string_view path_b, int context) {
    const std::vector<std::string> left = to_line_vector(a);
    const std::vector<std::string> right = to_line_vector(b);
    const std::vector<DiffOp> ops = diff_items(left, right);

    // Раскладываем ops в плоский список строк с номерами, чтобы удобно резать хунки.
    struct Row {
        DiffType type;
        std::string text;
        std::size_t a_line;
        std::size_t b_line;
    };
    std::vector<Row> rows;
    rows.reserve(ops.size() * 2);
    std::size_t a_line = 1;
    std::size_t b_line = 1;
    for (const auto& op : ops) {
        for (auto line : line_views(op.text, true)) {
            Row row{op.type, std::string(line), a_line, b_line};
            rows.push_back(row);
            if (op.type != DiffType::Insert) ++a_line;
            if (op.type != DiffType::Delete) ++b_line;
        }
    }

    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].type != DiffType::Equal) changed.push_back(i);
    }
    if (changed.empty()) return {};

    std::string out;
    out.append("--- ").append(path_a).push_back('\n');
    out.append("+++ ").append(path_b).push_back('\n');

    if (context < 0) context = 0;
    std::size_t index = 0;
    while (index < changed.size()) {
        const std::size_t begin = changed[index] > static_cast<std::size_t>(context)
                                      ? changed[index] - static_cast<std::size_t>(context)
                                      : 0;
        std::size_t end = changed[index];
        std::size_t next = index;
        while (next + 1 < changed.size() &&
               changed[next + 1] <= end + static_cast<std::size_t>(context) * 2 + 1) {
            end = changed[next + 1];
            ++next;
        }
        const std::size_t stop =
            std::min(rows.size(), end + static_cast<std::size_t>(context) + 1);

        std::size_t a_count = 0;
        std::size_t b_count = 0;
        for (std::size_t i = begin; i < stop; ++i) {
            if (rows[i].type != DiffType::Insert) ++a_count;
            if (rows[i].type != DiffType::Delete) ++b_count;
        }
        std::size_t a_start = 0;
        std::size_t b_start = 0;
        for (std::size_t i = begin; i < stop; ++i) {
            if (rows[i].type != DiffType::Insert) { a_start = rows[i].a_line; break; }
        }
        for (std::size_t i = begin; i < stop; ++i) {
            if (rows[i].type != DiffType::Delete) { b_start = rows[i].b_line; break; }
        }

        out.append("@@ -");
        out.append(std::to_string(a_start));
        out.push_back(',');
        out.append(std::to_string(a_count));
        out.append(" +");
        out.append(std::to_string(b_start));
        out.push_back(',');
        out.append(std::to_string(b_count));
        out.append(" @@\n");

        for (std::size_t i = begin; i < stop; ++i) {
            out.push_back(diff_type_marker(rows[i].type));
            out.append(rows[i].text);
            out.push_back('\n');
        }
        index = next + 1;
    }
    return out;
}

std::string colorize_diff(std::string_view diff) {
    std::string out;
    out.reserve(diff.size() + diff.size() / 4);
    const auto lines = line_views(diff, true);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = lines[i];
        if (starts_with(line, "+++") || starts_with(line, "---")) {
            out.append(bold_text(line));
        } else if (starts_with(line, "@@")) {
            out.append(cyan_text(line));
        } else if (starts_with(line, "+")) {
            out.append(green_text(line));
        } else if (starts_with(line, "-")) {
            out.append(red_text(line));
        } else {
            out.append(line);
        }
        out.push_back('\n');
    }
    return out;
}

std::string side_by_side_diff(std::string_view a, std::string_view b, int width) {
    if (width < 20) width = 20;
    const int column = (width - 3) / 2;
    std::string out;
    std::string header_left = pad_right(truncate_visible("старое", column), column);
    std::string header_right = pad_right(truncate_visible("новое", column), column);
    out.append(dim_text(header_left)).append(" │ ").append(dim_text(header_right));
    out.push_back('\n');

    const std::vector<std::string> left = to_line_vector(a);
    const std::vector<std::string> right = to_line_vector(b);
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < left.size() || j < right.size()) {
        std::string left_cell;
        std::string right_cell;
        std::string_view left_text = i < left.size() ? std::string_view(left[i]) : std::string_view{};
        std::string_view right_text = j < right.size() ? std::string_view(right[j]) : std::string_view{};
        if (i < left.size() && j < right.size() && left[i] == right[j]) {
            left_cell = std::string(truncate_visible(left_text, column));
            right_cell = std::string(truncate_visible(right_text, column));
            ++i;
            ++j;
        } else if (i < left.size() && (j >= right.size() || (j + 1 < right.size() && left[i] != right[j]))) {
            left_cell = std::string(truncate_visible(left_text, column));
            ++i;
        } else if (j < right.size()) {
            right_cell = std::string(truncate_visible(right_text, column));
            ++j;
        }
        std::string padded_left = pad_right(left_cell, column);
        if (!left_cell.empty()) padded_left = red_text(padded_left);
        std::string padded_right = pad_right(right_cell, column);
        if (!right_cell.empty()) padded_right = green_text(padded_right);
        out.append(padded_left).append(" │ ").append(padded_right);
        out.push_back('\n');
    }
    return out;
}

std::optional<std::string> apply_unified_diff(std::string_view source, std::string_view patch,
                                              std::string* error) {
    auto fail = [error](std::string message) -> std::optional<std::string> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };

    std::vector<std::string> lines = to_line_vector(source);
    const auto patch_lines = line_views(patch, true);

    std::size_t patch_index = 0;
    while (patch_index < patch_lines.size() && !starts_with(patch_lines[patch_index], "@@")) {
        ++patch_index;   // пропускаем заголовки ---/+++
    }
    if (patch_index >= patch_lines.size()) return fail("в патче нет ни одного хунка (@@)");

    std::vector<std::string> result;
    std::size_t source_index = 0;

    while (patch_index < patch_lines.size()) {
        const HunkHeader header = parse_hunk_header(patch_lines[patch_index]);
        if (!header.valid) return fail("не разобран заголовок хунка: " +
                                       std::string(patch_lines[patch_index]));
        ++patch_index;

        const std::size_t target_line = header.a_start == 0 ? 0 : header.a_start - 1;
        if (target_line < source_index) {
            return fail("хунки идут не по порядку: ожидалась строка " +
                        std::to_string(source_index + 1) + ", а в патче " +
                        std::to_string(target_line + 1));
        }
        while (source_index < target_line && source_index < lines.size()) {
            result.push_back(lines[source_index]);
            ++source_index;
        }

        while (patch_index < patch_lines.size() &&
               !starts_with(patch_lines[patch_index], "@@")) {
            const std::string_view raw = patch_lines[patch_index];
            ++patch_index;
            if (raw.empty()) continue;
            if (starts_with(raw, "\\")) continue;   // «\ No newline at end of file»
            const char marker = raw[0];
            const std::string_view text = raw.substr(1);

            if (marker == ' ') {
                if (source_index >= lines.size()) {
                    return fail("в файле не хватает строки контекста: «" + std::string(text) + "»");
                }
                if (lines[source_index] != text) {
                    if (!fuzzy_equal(lines[source_index], text)) {
                        return fail("контекст не совпал: в файле «" + lines[source_index] +
                                    "», в патче «" + std::string(text) + "» (строка " +
                                    std::to_string(source_index + 1) + ")");
                    }
                }
                result.push_back(lines[source_index]);
                ++source_index;
                continue;
            }
            if (marker == '-') {
                if (source_index >= lines.size()) {
                    return fail("патч удаляет строку за пределами файла: «" + std::string(text) + "»");
                }
                if (lines[source_index] != text && !fuzzy_equal(lines[source_index], text)) {
                    return fail("удаляемая строка не найдена: «" + std::string(text) +
                                "» (файл: «" + lines[source_index] + "», строка " +
                                std::to_string(source_index + 1) + ")");
                }
                ++source_index;
                continue;
            }
            if (marker == '+') {
                result.push_back(std::string(text));
                continue;
            }
            return fail("непонятная строка патча: «" + std::string(raw) + "»");
        }
    }

    while (source_index < lines.size()) {
        result.push_back(lines[source_index]);
        ++source_index;
    }

    std::string out;
    for (std::size_t i = 0; i < result.size(); ++i) {
        out.append(result[i]);
        out.push_back('\n');
    }
    return out;
}

std::optional<std::string> apply_search_replace(std::string_view source, std::string_view search,
                                                std::string_view replace, std::string* error) {
    auto fail = [error](std::string message) -> std::optional<std::string> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (search.empty()) return fail("пустой фрагмент для поиска");

    const std::size_t found = find_block(source, search);
    if (found != kNpos) {
        std::string out;
        out.reserve(source.size() + replace.size());
        out.append(source.substr(0, found));
        out.append(replace);
        out.append(source.substr(found + search.size()));
        return out;
    }

    // Мягкий режим: сравниваем построчно без ведущих пробелов — модели часто
    // «забывают» про отступы, а падать из-за этого незачем.
    const auto source_lines = line_views(source, true);
    const auto search_lines = line_views(search, true);
    if (search_lines.empty()) return fail("пустой фрагмент для поиска");

    std::vector<std::string> normalized_source;
    normalized_source.reserve(source_lines.size());
    for (auto line : source_lines) normalized_source.emplace_back(trim_right(line));

    for (std::size_t start = 0; start + search_lines.size() <= source_lines.size(); ++start) {
        bool match = true;
        for (std::size_t i = 0; i < search_lines.size(); ++i) {
            const std::string_view left = trim_left(normalized_source[start + i]);
            const std::string_view right = trim_left(trim_right(search_lines[i]));
            if (left != right) {
                match = false;
                break;
            }
        }
        if (!match) continue;

        // Сохраняем относительные отступы. Считаем минимальный отступ найденного
        // блока и замены, разница — это «базовый сдвиг» файла. Так модель может
        // прислать фрагмент без отступов, а мы вернём его на правильное место.
        auto leading_spaces = [](std::string_view line) {
            std::size_t count = 0;
            while (count < line.size() && (line[count] == ' ' || line[count] == '\t')) ++count;
            return count;
        };
        std::size_t source_indent = kNpos;
        for (std::size_t i = 0; i < search_lines.size(); ++i) {
            const std::string_view line = source_lines[start + i];
            if (trim(line).empty()) continue;
            source_indent = std::min(source_indent, leading_spaces(line));
        }
        if (source_indent == kNpos) source_indent = 0;

        std::size_t replace_indent = kNpos;
        for (auto line : line_views(replace, true)) {
            if (trim(line).empty()) continue;
            replace_indent = std::min(replace_indent, leading_spaces(line));
        }
        if (replace_indent == kNpos) replace_indent = 0;

        std::string rebuilt;
        for (auto line : line_views(replace, true)) {
            if (trim(line).empty()) {
                rebuilt.push_back('\n');
                continue;
            }
            std::string_view body = line;
            if (source_indent > replace_indent) {
                rebuilt.append(source_indent - replace_indent, ' ');
            } else if (replace_indent > source_indent) {
                std::size_t remove = replace_indent - source_indent;
                while (remove > 0 && !body.empty() && (body.front() == ' ' || body.front() == '\t')) {
                    body.remove_prefix(1);
                    --remove;
                }
            }
            rebuilt.append(body);
            rebuilt.push_back('\n');
        }

        std::string out;
        for (std::size_t i = 0; i < start; ++i) {
            out.append(source_lines[i]);
            out.push_back('\n');
        }
        out.append(rebuilt);
        for (std::size_t i = start + search_lines.size(); i < source_lines.size(); ++i) {
            out.append(source_lines[i]);
            out.push_back('\n');
        }
        return out;
    }

    std::string hint = "фрагмент не найден (";
    hint += std::to_string(search_lines.size());
    hint += " строк). Проверь пробелы и переводы строк; первые 80 символов: «";
    hint += std::string(search.substr(0, std::min<std::size_t>(80, search.size())));
    hint += "»";
    return fail(hint);
}

std::size_t find_block(std::string_view source, std::string_view block) noexcept {
    if (block.empty() || source.size() < block.size()) return kNpos;

    const std::size_t direct = source.find(block);
    if (direct != kNpos) return direct;

    // Пробуем с нормализованными переводами строк (патч CRLF против LF-файла).
    const std::string normalized_block = normalize_newlines(block);
    if (normalized_block != block) {
        const std::size_t second = source.find(normalized_block);
        if (second != kNpos) return second;
    }
    return kNpos;
}

bool fuzzy_equal(std::string_view a, std::string_view b) noexcept {
    std::size_t i = 0;
    std::size_t j = 0;
    while (true) {
        while (i < a.size() && is_ascii_space(a[i])) ++i;
        while (j < b.size() && is_ascii_space(b[j])) ++j;
        if (i >= a.size() || j >= b.size()) break;
        if (ascii_lower(a[i]) != ascii_lower(b[j])) return false;
        ++i;
        ++j;
    }
    while (i < a.size() && is_ascii_space(a[i])) ++i;
    while (j < b.size() && is_ascii_space(b[j])) ++j;
    return i >= a.size() && j >= b.size();
}

std::string similarity_report(std::string_view a, std::string_view b) {
    const std::vector<DiffOp> ops = diff_lines(a, b);
    const DiffStats stats = diff_stats(ops);
    const double ratio = similarity_ratio(a, b);

    std::vector<std::vector<std::string>> rows = {
        {"Строк добавлено", std::to_string(stats.added)},
        {"Строк удалено", std::to_string(stats.removed)},
        {"Строк без изменений", std::to_string(stats.unchanged)},
        {"Похожесть (LCS)", format_percent(ratio, 1)},
        {"Итог", stats.identical() ? "тексты идентичны" : stats.summary()},
    };
    std::string out = table(rows, {"Показатель", "Значение"});

    // Показываем первые расхождения: этого достаточно, чтобы понять, о чём они.
    std::size_t shown = 0;
    for (const auto& op : ops) {
        if (op.type == DiffType::Equal) continue;
        if (shown >= 6) break;
        out.push_back('\n');
        out.append(diff_type_marker(op.type) == '+' ? green_text("+ ") : red_text("- "));
        out.append(clip(op.text, 120));
        ++shown;
    }
    return out;
}


}  // namespace aia::str
