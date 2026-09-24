// ============================================================================
//  Текстовое ядро: строки, Unicode, кодировки, хеши, сравнение, таблицы, diff.
//
//  Это фундамент, на котором стоит всё остальное: и разбор ответов модели, и
//  рендер в консоли, и правка файлов. Поэтому здесь нет ничего «примерного» —
//  каждая функция используется хотя бы одним модулем проекта или тестом.
//
//  Правила модуля:
//    * всё в namespace aia::str, без глобального состояния;
//    * функции помечены [[nodiscard]] — молча терять результат нельзя;
//    * ошибки, где они возможны, отдаются через std::optional<...> либо через
//      out-параметр std::string* error, а не через исключения;
//    * никаких зависимостей кроме стандартной библиотеки.
// ============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace aia::str {

// ===========================================================================
//  1. ASCII и базовая обработка строк
// ===========================================================================
[[nodiscard]] constexpr bool is_ascii(char c) noexcept {
    return static_cast<unsigned char>(c) < 0x80U;
}
[[nodiscard]] constexpr bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
[[nodiscard]] constexpr bool is_ascii_digit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] constexpr bool is_ascii_alpha(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] constexpr bool is_ascii_alnum(char c) noexcept {
    return is_ascii_alpha(c) || is_ascii_digit(c);
}
[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] constexpr char ascii_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}
[[nodiscard]] int ascii_digit_value(char c) noexcept;   // 'f'→15, иначе -1

[[nodiscard]] std::string lower_ascii(std::string_view s);
[[nodiscard]] std::string upper_ascii(std::string_view s);
[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] bool istarts_with_ascii(std::string_view s, std::string_view prefix) noexcept;
[[nodiscard]] bool iends_with_ascii(std::string_view s, std::string_view suffix) noexcept;
[[nodiscard]] bool icontains_ascii(std::string_view s, std::string_view needle) noexcept;
[[nodiscard]] int icompare_ascii(std::string_view a, std::string_view b) noexcept;

[[nodiscard]] bool starts_with(std::string_view s, std::string_view prefix) noexcept;
[[nodiscard]] bool ends_with(std::string_view s, std::string_view suffix) noexcept;
[[nodiscard]] bool contains(std::string_view s, std::string_view needle) noexcept;
[[nodiscard]] std::size_t count_occurrences(std::string_view s, std::string_view needle) noexcept;

[[nodiscard]] std::string_view trim(std::string_view s) noexcept;
[[nodiscard]] std::string_view trim_left(std::string_view s) noexcept;
[[nodiscard]] std::string_view trim_right(std::string_view s) noexcept;
[[nodiscard]] std::string_view trim_chars(std::string_view s, std::string_view set) noexcept;
[[nodiscard]] std::string_view trim_quotes(std::string_view s) noexcept;
[[nodiscard]] std::string collapse_spaces(std::string_view s);
void trim_inplace(std::string& s);

[[nodiscard]] std::vector<std::string> split(std::string_view s, char sep, bool keep_empty = true);
[[nodiscard]] std::vector<std::string> split(std::string_view s, std::string_view sep,
                                             bool keep_empty = true);
[[nodiscard]] std::vector<std::string> split_any(std::string_view s, std::string_view separators,
                                                 bool keep_empty = false);
[[nodiscard]] std::vector<std::string> split_ws(std::string_view s);
[[nodiscard]] std::vector<std::string> split_lines(std::string_view s, bool keep_empty = true);
[[nodiscard]] std::vector<std::string_view> split_views(std::string_view s, char sep,
                                                        bool keep_empty = true);
[[nodiscard]] std::vector<std::string_view> line_views(std::string_view s, bool keep_empty = true);
[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view sep);
[[nodiscard]] std::string join(const std::vector<std::string_view>& parts, std::string_view sep);

[[nodiscard]] std::string replace_all(std::string_view s, std::string_view from, std::string_view to);
[[nodiscard]] std::string replace_first(std::string_view s, std::string_view from, std::string_view to);
[[nodiscard]] std::string remove_prefix(std::string_view s, std::string_view prefix);
[[nodiscard]] std::string remove_suffix(std::string_view s, std::string_view suffix);
[[nodiscard]] std::string repeat(std::string_view unit, std::size_t times);
[[nodiscard]] std::string reverse_bytes(std::string_view s);

[[nodiscard]] std::string pad_left(std::string_view s, int width, char fill = ' ');
[[nodiscard]] std::string pad_right(std::string_view s, int width, char fill = ' ');
[[nodiscard]] std::string center_text(std::string_view s, int width, char fill = ' ');
[[nodiscard]] std::string indent(std::string_view s, std::string_view prefix);
[[nodiscard]] std::string indent_lines(std::string_view s, std::string_view prefix,
                                       std::string_view skip_prefix = "");
[[nodiscard]] std::string dedent(std::string_view s);
[[nodiscard]] std::string normalize_newlines(std::string_view s);
[[nodiscard]] std::string ensure_trailing_newline(std::string_view s);
[[nodiscard]] std::string strip_bom(std::string_view s);

// ===========================================================================
//  2. Числа: разбор и форматирование
// ===========================================================================
/// Разбор целого. base = 0 — автоопределение по префиксу (0x/0b/0o), иначе — указанное.
[[nodiscard]] std::optional<long long> parse_int(std::string_view s, int base = 0) noexcept;
[[nodiscard]] std::optional<unsigned long long> parse_uint(std::string_view s,
                                                           int base = 0) noexcept;
[[nodiscard]] std::optional<double> parse_double(std::string_view s) noexcept;
[[nodiscard]] std::optional<bool> parse_bool(std::string_view s) noexcept;
/// Понимает «1024», «1.5 KiB», «2 MB», «3g», «10 мб». Возвращает байты.
[[nodiscard]] std::optional<std::uint64_t> parse_size(std::string_view s) noexcept;
/// Понимает «50», «50%», «1/3».
[[nodiscard]] std::optional<double> parse_ratio(std::string_view s) noexcept;

[[nodiscard]] std::string to_string(long long value);
[[nodiscard]] std::string to_string(unsigned long long value);
[[nodiscard]] std::string to_string(int value);
[[nodiscard]] std::string to_string(unsigned value);
[[nodiscard]] std::string to_string(bool value);
[[nodiscard]] std::string to_string(char value);
[[nodiscard]] std::string to_string(double value);
[[nodiscard]] std::string to_string(float value);

[[nodiscard]] std::string format_double(double value, int precision = 6, bool trim_zeros = true);
[[nodiscard]] std::string format_int_grouped(long long value, char separator = ' ');
[[nodiscard]] std::string format_size(std::uint64_t bytes, bool binary = true, int precision = 1);
[[nodiscard]] std::string format_duration(double seconds);
[[nodiscard]] std::string format_percent(double ratio, int precision = 0);
[[nodiscard]] std::string format_delta(long long value);   // +12 / -3 / 0

/// Русские склонения: 1 файл / 2 файла / 5 файлов.
[[nodiscard]] std::string plural_ru(long long n, std::string_view one, std::string_view few,
                                    std::string_view many, bool with_number = true);
[[nodiscard]] std::string plural_en(long long n, std::string_view one, std::string_view many,
                                    bool with_number = true);

// --- Шаблонный format ------------------------------------------------------
namespace detail {
template <typename T>
inline constexpr bool always_false_v = false;
}  // namespace detail

/// Преобразование аргумента format() в строку.
template <typename T>
[[nodiscard]] std::string arg_to_string(const T& value) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, std::string>) {
        return value;
    } else if constexpr (std::is_same_v<U, std::string_view>) {
        return std::string(value);
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        return value == nullptr ? std::string() : std::string(value);
    } else if constexpr (std::is_same_v<U, bool>) {
        return to_string(value);
    } else if constexpr (std::is_same_v<U, char>) {
        return std::string(1, value);
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (std::is_signed_v<U>) return to_string(static_cast<long long>(value));
        else return to_string(static_cast<unsigned long long>(value));
    } else if constexpr (std::is_floating_point_v<U>) {
        return format_double(static_cast<double>(value));
    } else if constexpr (std::is_convertible_v<U, std::string_view>) {
        return std::string(static_cast<std::string_view>(value));
    } else {
        static_assert(detail::always_false_v<U>,
                      "arg_to_string: тип не поддерживается. Приведи его к строке сам.");
    }
}

/// Подстановка по шаблону: «{}» по порядку, «{0}» по номеру, «{{}}» → «{}».
[[nodiscard]] std::string format_args(std::string_view pattern,
                                      const std::vector<std::string>& args);

template <typename... Args>
[[nodiscard]] std::string format(std::string_view pattern, const Args&... args) {
    if constexpr (sizeof...(args) == 0) {
        return format_args(pattern, {});
    } else {
        return format_args(pattern, std::vector<std::string>{arg_to_string(args)...});
    }
}

// ===========================================================================
//  3. Unicode: UTF-8 / UTF-16, регистр, ширина, категории
// ===========================================================================
struct Utf8Error {
    std::size_t offset = 0;      // байтовое смещение сбоя
    std::string reason;          // человеческое описание
    [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] bool utf8_is_valid(std::string_view s, Utf8Error* error = nullptr);
[[nodiscard]] std::vector<std::uint32_t> utf8_decode(std::string_view s);
void utf8_append(std::string& out, std::uint32_t codepoint);
[[nodiscard]] std::string utf8_encode(std::uint32_t codepoint);
[[nodiscard]] std::string utf8_from(const std::vector<std::uint32_t>& codepoints);
[[nodiscard]] std::size_t utf8_length(std::string_view s) noexcept;         // число кодпоинтов
[[nodiscard]] std::size_t utf8_byte_offset(std::string_view s, std::size_t index) noexcept;
[[nodiscard]] std::size_t utf8_codepoint_at(std::string_view s, std::size_t index) noexcept;
[[nodiscard]] std::string_view utf8_substr(std::string_view s, std::size_t start,
                                           std::size_t count = std::string_view::npos) noexcept;
[[nodiscard]] std::size_t utf8_offset_to_line_column(std::string_view s, std::size_t byte_offset,
                                                     std::size_t* line, std::size_t* column) noexcept;
[[nodiscard]] std::u16string utf8_to_utf16(std::string_view s);
[[nodiscard]] std::string utf16_to_utf8(std::u16string_view s);
[[nodiscard]] std::wstring utf8_to_wide(std::string_view s);
[[nodiscard]] std::string wide_to_utf8(std::wstring_view s);
/// Для кодировок Windows-1251 (кириллица) — нужна при чтении старых файлов.
[[nodiscard]] std::string cp1251_to_utf8(std::string_view s);
[[nodiscard]] std::string utf8_to_cp1251(std::string_view s);

enum class Category : std::uint8_t { Other, Control, Space, Digit, Letter, Mark,
                                     Punctuation, Symbol, Separator };
[[nodiscard]] std::string_view category_name(Category c) noexcept;
[[nodiscard]] Category category_of(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_letter_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_digit_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_space_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_combining_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_word_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_latin_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_cyrillic_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_greek_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_cjk_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_emoji_cp(std::uint32_t cp) noexcept;
[[nodiscard]] bool is_private_use_cp(std::uint32_t cp) noexcept;
[[nodiscard]] std::string_view script_of(std::uint32_t cp) noexcept;

[[nodiscard]] std::uint32_t to_lower_cp(std::uint32_t cp) noexcept;
[[nodiscard]] std::uint32_t to_upper_cp(std::uint32_t cp) noexcept;
[[nodiscard]] std::string lower_unicode(std::string_view s);
[[nodiscard]] std::string upper_unicode(std::string_view s);
[[nodiscard]] std::string capitalize(std::string_view s);
[[nodiscard]] std::string title_case(std::string_view s);

[[nodiscard]] int codepoint_width(std::uint32_t cp) noexcept;
[[nodiscard]] int display_width(std::string_view s) noexcept;              // без ANSI, с CJK
[[nodiscard]] std::size_t grapheme_count(std::string_view s) noexcept;
[[nodiscard]] std::string remove_accents(std::string_view s);
[[nodiscard]] std::string transliterate(std::string_view s);
[[nodiscard]] std::string utf8_truncate(std::string_view s, int max_width,
                                        std::string_view ellipsis = "…");
[[nodiscard]] std::string_view utf8_slice_columns(std::string_view s, int start_column,
                                                  int width) noexcept;
[[nodiscard]] std::string highlight_position(std::string_view s, std::size_t codepoint_index,
                                             std::string_view marker = "▲");
/// Найти подстроку без учёта регистра и диакритики (латиница/кириллица).
[[nodiscard]] std::optional<std::size_t> find_nocase(std::string_view haystack,
                                                     std::string_view needle);

// ===========================================================================
//  4. ANSI: цвета и стили (нужны консоли и рендеру)
// ===========================================================================
namespace ansi {
inline constexpr std::string_view reset     = "\x1b[0m";
inline constexpr std::string_view bold      = "\x1b[1m";
inline constexpr std::string_view dim       = "\x1b[2m";
inline constexpr std::string_view italic    = "\x1b[3m";
inline constexpr std::string_view underline = "\x1b[4m";
inline constexpr std::string_view blink     = "\x1b[5m";
inline constexpr std::string_view reverse   = "\x1b[7m";
inline constexpr std::string_view strike    = "\x1b[9m";

inline constexpr std::string_view black   = "\x1b[30m";
inline constexpr std::string_view red     = "\x1b[31m";
inline constexpr std::string_view green   = "\x1b[32m";
inline constexpr std::string_view yellow  = "\x1b[33m";
inline constexpr std::string_view blue    = "\x1b[34m";
inline constexpr std::string_view magenta = "\x1b[35m";
inline constexpr std::string_view cyan    = "\x1b[36m";
inline constexpr std::string_view white   = "\x1b[37m";
inline constexpr std::string_view gray    = "\x1b[90m";

inline constexpr std::string_view bright_red     = "\x1b[91m";
inline constexpr std::string_view bright_green   = "\x1b[92m";
inline constexpr std::string_view bright_yellow  = "\x1b[93m";
inline constexpr std::string_view bright_blue    = "\x1b[94m";
inline constexpr std::string_view bright_magenta = "\x1b[95m";
inline constexpr std::string_view bright_cyan    = "\x1b[96m";
inline constexpr std::string_view bright_white   = "\x1b[97m";

inline constexpr std::string_view bg_red    = "\x1b[41m";
inline constexpr std::string_view bg_green  = "\x1b[42m";
inline constexpr std::string_view bg_yellow = "\x1b[43m";
inline constexpr std::string_view bg_blue   = "\x1b[44m";
inline constexpr std::string_view bg_gray   = "\x1b[100m";
inline constexpr std::string_view bg_white  = "\x1b[107m";
}  // namespace ansi

[[nodiscard]] std::string colorize(std::string_view text, std::string_view code);
[[nodiscard]] std::string rgb_colorize(std::string_view text, int r, int g, int b);
[[nodiscard]] std::string bold_text(std::string_view s);
[[nodiscard]] std::string dim_text(std::string_view s);
[[nodiscard]] std::string red_text(std::string_view s);
[[nodiscard]] std::string green_text(std::string_view s);
[[nodiscard]] std::string yellow_text(std::string_view s);
[[nodiscard]] std::string blue_text(std::string_view s);
[[nodiscard]] std::string cyan_text(std::string_view s);
[[nodiscard]] std::string magenta_text(std::string_view s);
[[nodiscard]] std::string gray_text(std::string_view s);
[[nodiscard]] bool has_ansi(std::string_view s) noexcept;
[[nodiscard]] std::string strip_ansi(std::string_view s);
/// Длина строки для терминала: ANSI не считается, wide-символы — за два.
[[nodiscard]] int terminal_width(std::string_view s) noexcept;
[[nodiscard]] std::string truncate_visible(std::string_view s, int max_width,
                                           std::string_view ellipsis = "…");

// ===========================================================================
//  5. Экранирование и кодирование
// ===========================================================================
[[nodiscard]] std::string escape_c(std::string_view s);
[[nodiscard]] std::string unescape_c(std::string_view s);
[[nodiscard]] std::string escape_json(std::string_view s);
[[nodiscard]] std::string unescape_json(std::string_view s);
[[nodiscard]] std::string escape_html(std::string_view s);
[[nodiscard]] std::string unescape_html(std::string_view s);
[[nodiscard]] std::string url_encode(std::string_view s, bool space_as_plus = false);
[[nodiscard]] std::optional<std::string> url_decode(std::string_view s);
[[nodiscard]] std::string url_path_join(std::string_view base, std::string_view relative);
[[nodiscard]] std::string base64_encode(std::string_view data);
[[nodiscard]] std::optional<std::string> base64_decode(std::string_view text);
[[nodiscard]] std::string base64url_encode(std::string_view data);
[[nodiscard]] std::optional<std::string> base64url_decode(std::string_view text);
[[nodiscard]] std::string hex_encode(std::string_view data, bool uppercase = false);
[[nodiscard]] std::optional<std::string> hex_decode(std::string_view text);
[[nodiscard]] std::string binary_dump(std::string_view data, std::size_t max_bytes = 256);

[[nodiscard]] std::string csv_quote(std::string_view field, char delimiter = ',', char quote = '"');
[[nodiscard]] std::vector<std::string> csv_parse_line(std::string_view line, char delimiter = ',');
[[nodiscard]] std::string csv_build(const std::vector<std::vector<std::string>>& rows,
                                    char delimiter = ',');

[[nodiscard]] std::string shell_quote_posix(std::string_view argument);
[[nodiscard]] std::string shell_quote_windows(std::string_view argument);
[[nodiscard]] std::string shell_join_posix(const std::vector<std::string>& arguments);
[[nodiscard]] std::vector<std::string> split_command_line(std::string_view text);
[[nodiscard]] std::string sanitize_filename(std::string_view name, char replacement = '_');
[[nodiscard]] std::string slugify(std::string_view text, char separator = '-');
[[nodiscard]] std::string normalize_path(std::string_view path);
[[nodiscard]] bool path_is_absolute(std::string_view path) noexcept;
[[nodiscard]] std::string path_join(std::string_view a, std::string_view b);
[[nodiscard]] std::string path_filename(std::string_view path);
[[nodiscard]] std::string path_stem(std::string_view path);
[[nodiscard]] std::string path_extension(std::string_view path);
[[nodiscard]] std::string path_parent(std::string_view path);
[[nodiscard]] std::vector<std::string> path_parts(std::string_view path);
[[nodiscard]] std::string path_relative(std::string_view path, std::string_view base);
[[nodiscard]] bool path_is_inside(std::string_view path, std::string_view directory);

// ===========================================================================
//  6. Хеши и случайность (для кешей, ключей сессий, дедупликации)
// ===========================================================================
[[nodiscard]] std::uint64_t fnv1a64(std::string_view data) noexcept;
[[nodiscard]] std::uint32_t crc32(std::string_view data) noexcept;
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view data) noexcept;
[[nodiscard]] std::string sha256_hex(std::string_view data);
[[nodiscard]] std::string short_hash(std::string_view data, std::size_t hex_chars = 8);
[[nodiscard]] std::string random_token(std::size_t bytes = 16, bool hex_only = true);
[[nodiscard]] std::string random_hex(std::size_t bytes);
[[nodiscard]] std::uint64_t stable_hash(std::string_view data) noexcept;   // = fnv1a64

// ===========================================================================
//  7. Сравнение, нечёткий поиск, wildcard, natural sort
// ===========================================================================
[[nodiscard]] std::size_t levenshtein(std::string_view a, std::string_view b,
                                      std::size_t limit = static_cast<std::size_t>(-1));
[[nodiscard]] std::size_t damerau_levenshtein(std::string_view a, std::string_view b,
                                              std::size_t limit = static_cast<std::size_t>(-1));
[[nodiscard]] double jaro(std::string_view a, std::string_view b);
[[nodiscard]] double jaro_winkler(std::string_view a, std::string_view b,
                                  double prefix_scale = 0.1);
[[nodiscard]] std::size_t lcs_length(std::string_view a, std::string_view b);
[[nodiscard]] double similarity_ratio(std::string_view a, std::string_view b);
[[nodiscard]] int fuzzy_score(std::string_view query, std::string_view candidate);
[[nodiscard]] std::vector<std::size_t> fuzzy_positions(std::string_view query,
                                                       std::string_view candidate);
[[nodiscard]] std::optional<std::string> best_match(std::string_view query,
                                                    const std::vector<std::string>& candidates,
                                                    int min_score = 40);
[[nodiscard]] std::vector<std::pair<std::string, int>>
rank_matches(std::string_view query, const std::vector<std::string>& candidates,
             std::size_t limit = 10, int min_score = 20);
[[nodiscard]] int natural_compare(std::string_view a, std::string_view b);
[[nodiscard]] bool natural_less(std::string_view a, std::string_view b);
[[nodiscard]] bool wildcard_match(std::string_view pattern, std::string_view text,
                                  bool case_sensitive = true) noexcept;
[[nodiscard]] std::string glob_to_regex(std::string_view pattern, bool case_sensitive = true);

// ===========================================================================
//  8. Текст, таблицы, разметка
// ===========================================================================
[[nodiscard]] std::string wrap(std::string_view text, int width);
[[nodiscard]] std::string wrap_block(std::string_view text, int width);
[[nodiscard]] std::string clip(std::string_view s, int max_width, std::string_view ellipsis = "…");
[[nodiscard]] std::string clip_middle(std::string_view s, int max_width,
                                      std::string_view ellipsis = "…");
[[nodiscard]] std::string clip_lines(std::string_view s, std::size_t max_lines,
                                     std::string_view ellipsis = "…");
[[nodiscard]] std::string numbered_lines(std::string_view s, int start = 1, int number_width = 0);
[[nodiscard]] std::string table(const std::vector<std::vector<std::string>>& rows,
                                const std::vector<std::string>& headers = {},
                                int max_width = 0);
[[nodiscard]] std::string box(std::string_view title, std::string_view body, int width = 0);
[[nodiscard]] std::string progress_bar(double fraction, int width = 30,
                                       bool with_percent = true);
[[nodiscard]] std::string spinner_frame(std::size_t tick);
[[nodiscard]] std::string markdown_to_ansi(std::string_view markdown);
[[nodiscard]] std::string strip_markdown(std::string_view markdown);
[[nodiscard]] std::string highlight(std::string_view text,
                                    const std::vector<std::string>& terms,
                                    std::string_view code = ansi::bright_yellow);
[[nodiscard]] std::string render_template(std::string_view tmpl,
                                          const std::map<std::string, std::string>& vars);
[[nodiscard]] std::vector<std::string> template_variables(std::string_view tmpl);

struct TextStats {
    std::size_t bytes = 0;
    std::size_t codepoints = 0;
    std::size_t graphemes = 0;
    std::size_t words = 0;
    std::size_t lines = 0;
    std::size_t sentences = 0;
    std::size_t long_lines = 0;      // длиннее 120 колонок
    std::size_t max_line_width = 0;
    double average_line_width = 0.0;
    bool has_tabs = false;
    bool has_crlf = false;
    bool has_trailing_space = false;
};
[[nodiscard]] TextStats text_stats(std::string_view text);
[[nodiscard]] std::string text_stats_report(std::string_view text);
[[nodiscard]] std::size_t estimate_tokens(std::string_view text);
[[nodiscard]] std::vector<std::string> sentences(std::string_view text);
[[nodiscard]] std::vector<std::string> words(std::string_view text);
[[nodiscard]] std::vector<std::pair<std::string, int>> word_frequencies(std::string_view text);
[[nodiscard]] std::vector<std::pair<std::string, int>> key_terms(std::string_view text,
                                                                 std::size_t limit = 12);
[[nodiscard]] std::string keyword_summary(std::string_view text, std::size_t keywords = 8,
                                          std::size_t sentences_limit = 3);
[[nodiscard]] bool looks_like_code(std::string_view text) noexcept;
[[nodiscard]] std::string detect_language_hint(std::string_view path);

// ===========================================================================
//  9. Diff: сравнение текстов и патчи
// ===========================================================================
enum class DiffType : std::uint8_t { Equal, Insert, Delete };

[[nodiscard]] std::string_view diff_type_name(DiffType type) noexcept;
[[nodiscard]] char diff_type_marker(DiffType type) noexcept;

struct DiffOp {
    DiffType type = DiffType::Equal;
    std::string text;
};

struct DiffStats {
    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t unchanged = 0;
    [[nodiscard]] std::size_t total() const noexcept { return added + removed + unchanged; }
    [[nodiscard]] bool identical() const noexcept { return added == 0 && removed == 0; }
    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] std::vector<DiffOp> diff_lines(std::string_view a, std::string_view b);
[[nodiscard]] std::vector<DiffOp> diff_chars(std::string_view a, std::string_view b);
[[nodiscard]] std::vector<DiffOp> diff_words(std::string_view a, std::string_view b);
[[nodiscard]] DiffStats diff_stats(const std::vector<DiffOp>& ops);
[[nodiscard]] std::string unified_diff(std::string_view a, std::string_view b,
                                       std::string_view path_a = "a", std::string_view path_b = "b",
                                       int context = 3);
[[nodiscard]] std::string colorize_diff(std::string_view diff);
[[nodiscard]] std::string side_by_side_diff(std::string_view a, std::string_view b, int width = 100);
[[nodiscard]] std::optional<std::string> apply_unified_diff(std::string_view source,
                                                            std::string_view patch,
                                                            std::string* error = nullptr);
[[nodiscard]] std::optional<std::string> apply_search_replace(std::string_view source,
                                                              std::string_view search,
                                                              std::string_view replace,
                                                              std::string* error = nullptr);
[[nodiscard]] std::size_t find_block(std::string_view source, std::string_view block) noexcept;
[[nodiscard]] bool fuzzy_equal(std::string_view a, std::string_view b) noexcept;  // без пробелов/регистра
[[nodiscard]] std::string similarity_report(std::string_view a, std::string_view b);

}  // namespace aia::str
