// ============================================================================
//  Тесты текстового ядра: строки, числа, Unicode, кодировки, сравнение, diff.
//
//  Тесты пишутся не «для галочки»: каждый блок strings.cpp проверяется на
//  граничных случаях, которые реально встречаются в работе агента (CRLF,
//  смешанные разделители путей, обрыв UTF-8, длинные пути, кириллица).
// ============================================================================
#include "core/strings.hpp"
#include "testing.hpp"

#include <string>
#include <vector>

using namespace aia;
using namespace aia::test;

// ===========================================================================
//  Раздел 1: базовые строки
// ===========================================================================
TEST(strings, ascii_helpers) {
    REQUIRE_EQ(str::ascii_digit_value('7'), 7);
    REQUIRE_EQ(str::ascii_digit_value('f'), 15);
    REQUIRE_EQ(str::ascii_digit_value('F'), 15);
    REQUIRE_EQ(str::ascii_digit_value('z'), -1);
    REQUIRE(str::is_ascii_space('\n'));
    REQUIRE(str::is_ascii_space(' '));
    REQUIRE(!str::is_ascii_space('a'));
    REQUIRE(str::is_ascii_digit('5'));
    REQUIRE(!str::is_ascii_digit('x'));
    // ASCII-функции не трогают UTF-8: байты кириллицы остаются как есть.
    REQUIRE_EQ(str::lower_ascii("ЖУК"), "ЖУК");
    REQUIRE_EQ(str::lower_ascii("ЖУК abc"), "ЖУК abc");
    REQUIRE_EQ(str::ascii_upper('z'), 'Z');
}

TEST(strings, case_conversion_ascii) {
    REQUIRE_EQ(str::lower_ascii("Привет WORLD"), "Привет world");
    REQUIRE_EQ(str::upper_ascii("abcЖ"), "ABCЖ");
    REQUIRE(str::iequals_ascii("Debug", "DEBUG"));
    REQUIRE(str::istarts_with_ascii("README.md", "readme"));
    REQUIRE(str::iends_with_ascii("Makefile", "FILE"));
    REQUIRE(str::icontains_ascii("src/Core/Strings.cpp", "core"));
    REQUIRE_EQ(str::icompare_ascii("abc", "ABC"), 0);
    REQUIRE(str::icompare_ascii("abc", "abd") < 0);
}

TEST(strings, trim_and_collapse) {
    REQUIRE_EQ(str::trim("  привет  "), "привет");
    REQUIRE_EQ(str::trim_left("\t\n x"), "x");
    REQUIRE_EQ(str::trim_right("x \r\n"), "x");
    REQUIRE_EQ(str::trim_chars("...текст...", "."), "текст");
    REQUIRE_EQ(str::trim_quotes("\"строка\""), "строка");
    REQUIRE_EQ(str::trim_quotes("\"незакрытая"), "\"незакрытая");
    REQUIRE_EQ(str::collapse_spaces("a   b\n\n c"), "a b c");

    std::string value = "  обрезка  ";
    str::trim_inplace(value);
    REQUIRE_EQ(value, "обрезка");
}

TEST(strings, split_and_join) {
    const auto parts = str::split("a,b,,c", ',');
    REQUIRE_EQ(parts.size(), std::size_t{4});
    REQUIRE_EQ(parts[2], "");

    const auto compact = str::split("a,b,,c", ',', false);
    REQUIRE_EQ(compact.size(), std::size_t{3});

    const auto by_word = str::split("ключ = значение", " = ");
    REQUIRE_EQ(by_word.size(), std::size_t{2});
    REQUIRE_EQ(by_word[1], "значение");

    const auto ws = str::split_ws("  один   два\tтри\n");
    REQUIRE_EQ(ws.size(), std::size_t{3});
    REQUIRE_EQ(ws[2], "три");

    const auto lines = str::split_lines("a\nb\n\nc");
    REQUIRE_EQ(lines.size(), std::size_t{4});

    REQUIRE_EQ(str::join(std::vector<std::string>{"a", "b", "c"}, ", "), "a, b, c");
    REQUIRE_EQ(str::join(std::vector<std::string_view>{"x", "y"}, "+"), "x+y");
    REQUIRE_EQ(str::join(std::vector<std::string>{}, ","), "");

    const auto mixed = str::split_any("a;b|c d", ";| ");
    REQUIRE_EQ(mixed.size(), std::size_t{4});

    REQUIRE_EQ(str::line_views("a\r\nb", true).size(), std::size_t{2});
    REQUIRE_EQ(str::line_views("a\r\nb", true)[0], "a");   // \r отрезан
}

TEST(strings, replace_and_wrap_helpers) {
    REQUIRE_EQ(str::replace_all("aaa", "a", "bb"), "bbbbbb");
    REQUIRE_EQ(str::replace_all("путь/к/файлу", "/", "\\"), "путь\\к\\файлу");
    REQUIRE_EQ(str::replace_first("a.b.c", ".", "-"), "a-b.c");
    REQUIRE_EQ(str::remove_prefix("src/main.cpp", "src/"), "main.cpp");
    REQUIRE_EQ(str::remove_suffix("main.cpp", ".cpp"), "main");
    REQUIRE_EQ(str::repeat("ab", 3), "ababab");
    REQUIRE_EQ(str::repeat("x", 0), "");
    REQUIRE_EQ(str::count_occurrences("aaa", "aa"), std::size_t{1});   // без перекрытий
    REQUIRE_EQ(str::reverse_bytes("abc"), "cba");

    REQUIRE_EQ(str::pad_right("abc", 5, '.'), "abc..");
    REQUIRE_EQ(str::pad_left("7", 3, '0'), "007");
    REQUIRE_EQ(str::center_text("x", 5, '-'), "--x--");
    REQUIRE_EQ(str::indent("x", "  "), "  x");
    REQUIRE_EQ(str::indent_lines("a\nb", "> "), "> a\n> b");

    REQUIRE_EQ(str::dedent("    a\n      b\n"), "a\n  b\n");
    REQUIRE_EQ(str::normalize_newlines("a\r\nb\rc"), "a\nb\nc");
    REQUIRE_EQ(str::ensure_trailing_newline("x"), "x\n");
    REQUIRE_EQ(str::ensure_trailing_newline("x\n"), "x\n");
    REQUIRE_EQ(str::strip_bom("\xEF\xBB\xBFтекст"), "текст");
}

// ===========================================================================
//  Раздел 2: числа
// ===========================================================================
TEST(numbers, parse_int_bases) {
    REQUIRE_EQ(*str::parse_int("42"), 42LL);
    REQUIRE_EQ(*str::parse_int("-17"), -17LL);
    REQUIRE_EQ(*str::parse_int("0x1F"), 31LL);
    REQUIRE_EQ(*str::parse_int("0b1011"), 11LL);
    REQUIRE_EQ(*str::parse_int("0o17"), 15LL);
    REQUIRE_EQ(*str::parse_int("1_000_000"), 1000000LL);
    REQUIRE_EQ(*str::parse_int("  255  "), 255LL);
    REQUIRE_EQ(*str::parse_int("ff", 16), 255LL);
    REQUIRE(!str::parse_int("12abc").has_value());
    REQUIRE(!str::parse_int("").has_value());
    REQUIRE(!str::parse_int("12.5").has_value());
    REQUIRE_EQ(*str::parse_int("9223372036854775807"), 9223372036854775807LL);
    REQUIRE(!str::parse_int("9223372036854775808").has_value());   // переполнение
}

TEST(numbers, parse_uint_and_double) {
    REQUIRE_EQ(*str::parse_uint("0xFFFF"), 65535ULL);
    REQUIRE(!str::parse_uint("-1").has_value());
    REQUIRE_NEAR(*str::parse_double("3.14"), 3.14, 1e-9);
    REQUIRE_NEAR(*str::parse_double("3,14"), 3.14, 1e-9);      // «русская» запятая
    REQUIRE_NEAR(*str::parse_double("1e3"), 1000.0, 1e-9);
    REQUIRE(!str::parse_double("нет").has_value());
    REQUIRE(!str::parse_double("").has_value());
}

TEST(numbers, parse_bool_size_ratio) {
    REQUIRE_EQ(*str::parse_bool("Да"), true);
    REQUIRE_EQ(*str::parse_bool("OFF"), false);
    REQUIRE(!str::parse_bool("может").has_value());

    REQUIRE_EQ(*str::parse_size("1024"), 1024ULL);
    REQUIRE_EQ(*str::parse_size("1 KiB"), 1024ULL);
    REQUIRE_EQ(*str::parse_size("1.5 MiB"), 1572864ULL);
    REQUIRE_EQ(*str::parse_size("2mb"), 2000000ULL);
    REQUIRE_EQ(*str::parse_size("3 гб"), 3000000000ULL);
    REQUIRE(!str::parse_size("много").has_value());

    REQUIRE_NEAR(*str::parse_ratio("50%"), 0.5, 1e-9);
    REQUIRE_NEAR(*str::parse_ratio("1/4"), 0.25, 1e-9);
    REQUIRE_NEAR(*str::parse_ratio("0.75"), 0.75, 1e-9);
}

TEST(numbers, format_numbers) {
    REQUIRE_EQ(str::to_string(42), "42");
    REQUIRE_EQ(str::to_string(true), "true");
    REQUIRE_EQ(str::format_double(1.0 / 3.0, 3), "0.333");
    REQUIRE_EQ(str::format_double(2.5000, 3), "2.5");           // хвостовые нули убраны
    REQUIRE_EQ(str::format_double(2.5, 3, false), "2.500");
    REQUIRE_EQ(str::format_double(-0.0), "0");
    REQUIRE_EQ(str::format_int_grouped(1234567), "1 234 567");
    REQUIRE_EQ(str::format_int_grouped(-1234), "-1 234");
    REQUIRE_EQ(str::format_int_grouped(999), "999");
}

TEST(numbers, format_size_duration_plural) {
    REQUIRE_EQ(str::format_size(512), "512 Б");
    REQUIRE_EQ(str::format_size(2048), "2 КиБ");
    REQUIRE_EQ(str::format_size(1500000, false), "1.5 МБ");
    REQUIRE_EQ(str::format_duration(0.25), "250 мс");
    REQUIRE_EQ(str::format_duration(2.5), "2.5 с");
    REQUIRE_EQ(str::format_duration(90), "1 мин 30 с");
    REQUIRE_EQ(str::format_duration(3660), "1 ч 1 мин");
    REQUIRE_EQ(str::format_duration(-1), "—");

    REQUIRE_EQ(str::plural_ru(1, "файл", "файла", "файлов"), "1 файл");
    REQUIRE_EQ(str::plural_ru(2, "файл", "файла", "файлов"), "2 файла");
    REQUIRE_EQ(str::plural_ru(5, "файл", "файла", "файлов"), "5 файлов");
    REQUIRE_EQ(str::plural_ru(11, "файл", "файла", "файлов"), "11 файлов");
    REQUIRE_EQ(str::plural_ru(21, "файл", "файла", "файлов"), "21 файл");
    REQUIRE_EQ(str::plural_ru(101, "строка", "строки", "строк"), "101 строка");
    REQUIRE_EQ(str::plural_en(1, "file", "files", false), "file");
}

TEST(numbers, format_template) {
    REQUIRE_EQ(str::format("a={} b={}", 1, "два"), "a=1 b=два");
    REQUIRE_EQ(str::format("{1}-{0}", "первый", "второй"), "второй-первый");
    REQUIRE_EQ(str::format("{{литерал}} {}", "x"), "{литерал} x");
    REQUIRE_EQ(str::format("пусто {}"), "пусто {}");            // нет аргумента — не падаем
    REQUIRE_EQ(str::format("без аргументов"), "без аргументов");
    REQUIRE_EQ(str::format("{}", true), "true");
    REQUIRE_EQ(str::format("{:.2f}", 1.5), "{:.2f}");           // точность не поддерживаем
    REQUIRE_EQ(str::format("{}", 2.0), "2");
}

// ===========================================================================
//  Раздел 3: Unicode
// ===========================================================================
TEST(unicode, utf8_validity) {
    REQUIRE(str::utf8_is_valid("обычный текст"));
    REQUIRE(str::utf8_is_valid("emoji: 🚀 и CJK: 日本語"));
    REQUIRE(!str::utf8_is_valid("обрыв: \xE2\x82"));            // недописанный €
    REQUIRE(!str::utf8_is_valid("мусор: \xFF"));

    str::Utf8Error error;
    REQUIRE(!str::utf8_is_valid("ok \xC3\x28 bad", &error));
    REQUIRE_EQ(error.offset, std::size_t{3});
    REQUIRE_CONTAINS(error.to_string(), "позиция 3");
}

TEST(unicode, utf8_operations) {
    const std::string text = "Привет, мир!";
    REQUIRE_EQ(str::utf8_length(text), std::size_t{12});
    REQUIRE_EQ(str::utf8_codepoint_at(text, 0), 0x041FU);      // П
    REQUIRE_EQ(str::utf8_substr(text, 0, 6), "Привет");
    REQUIRE_EQ(str::utf8_substr(text, 8), "мир!");

    const std::string encoded = str::utf8_encode(0x1F680);     // 🚀
    REQUIRE_EQ(encoded.size(), std::size_t{4});
    REQUIRE_EQ(str::utf8_length(encoded), std::size_t{1});

    const auto codepoints = str::utf8_decode("Жук");
    REQUIRE_EQ(codepoints.size(), std::size_t{3});
    REQUIRE_EQ(str::utf8_from(codepoints), "Жук");

    // Суррогаты из UTF-16 не должны попадать в UTF-8.
    REQUIRE_EQ(str::utf8_encode(0xD800), "\xEF\xBF\xBD");
    REQUIRE_EQ(str::utf8_encode(0x110000), "\xEF\xBF\xBD");
}

TEST(unicode, utf16_and_wide) {
    const std::u16string utf16 = str::utf8_to_utf16("Привет 🚀");
    REQUIRE_EQ(utf16.size(), std::size_t{9});   // 7 знаков + суррогатная пара для 🚀
    REQUIRE_EQ(str::utf16_to_utf8(utf16), "Привет 🚀");
    REQUIRE_EQ(str::wide_to_utf8(str::utf8_to_wide("тест")), "тест");
}

TEST(unicode, cp1251_roundtrip) {
    // «Привет» в CP1251
    const std::string cp1251 = "\xCF\xF0\xE8\xE2\xE5\xF2";
    REQUIRE_EQ(str::cp1251_to_utf8(cp1251), "Привет");
    REQUIRE_EQ(str::utf8_to_cp1251("Привет"), cp1251);
    // utf8_to_cp1251 отдаёт БАЙТЫ CP1251 (не UTF-8): «текст» → F2 E5 EA F1 F2.
    REQUIRE_EQ(str::utf8_to_cp1251("текст 🚀"), "\xF2\xE5\xEA\xF1\xF2 ?");
}

TEST(unicode, categories_and_scripts) {
    REQUIRE_EQ(str::category_of('A'), str::Category::Letter);
    REQUIRE_EQ(str::category_of('7'), str::Category::Digit);
    REQUIRE_EQ(str::category_of(' '), str::Category::Space);
    REQUIRE_EQ(str::category_of('!'), str::Category::Punctuation);
    REQUIRE_EQ(str::category_of(0x0301), str::Category::Mark);   // комбинирующий акут
    REQUIRE_EQ(str::category_name(str::Category::Mark), "mark");

    REQUIRE(str::is_cyrillic_cp(0x0416));                        // Ж
    REQUIRE(!str::is_latin_cp(0x0416));
    REQUIRE(str::is_latin_cp(0x00E9));                           // é
    REQUIRE(str::is_greek_cp(0x03A9));                           // Ω
    REQUIRE(str::is_cjk_cp(0x4E2D));                             // 中
    REQUIRE(str::is_emoji_cp(0x1F600));
    REQUIRE_EQ(str::script_of(0x0416), "cyrillic");
    REQUIRE_EQ(str::script_of(0x1F600), "emoji");
    REQUIRE(str::is_word_cp(0x0451));                            // ё — часть слова
    REQUIRE(!str::is_word_cp('!'));
}

TEST(unicode, case_and_width) {
    REQUIRE_EQ(str::lower_unicode("ПРИВЕТ World"), "привет world");
    REQUIRE_EQ(str::upper_unicode("привет"), "ПРИВЕТ");
    REQUIRE_EQ(str::upper_unicode("ёж"), "ЁЖ");
    REQUIRE_EQ(str::capitalize("пРИВЕТ"), "Привет");
    REQUIRE_EQ(str::title_case("hello world"), "Hello World");

    REQUIRE_EQ(str::display_width("abc"), 3);
    REQUIRE_EQ(str::display_width("привет"), 6);
    REQUIRE_EQ(str::display_width("日本語"), 6);                  // CJK — по две колонки
    REQUIRE_EQ(str::display_width("a\u0301bc"), 3);               // комбинирующий знак — ноль
    REQUIRE_EQ(str::codepoint_width(0x1F680), 2);

    REQUIRE_EQ(str::grapheme_count("привет"), std::size_t{6});
    REQUIRE_EQ(str::remove_accents("café"), "cafe");
    REQUIRE_EQ(str::transliterate("Привет, мир!"), "Privet, mir!");
    REQUIRE_EQ(str::transliterate("Щука"), "Shchuka");
    REQUIRE_EQ(str::slugify("Привет, мир!"), "privet-mir");
    REQUIRE_EQ(str::slugify("  Много   пробелов  "), "mnogo-probelov");
}

TEST(unicode, truncate_and_search) {
    REQUIRE_EQ(str::utf8_truncate("привет", 3), "пр…");
    REQUIRE_EQ(str::utf8_truncate("привет", 20), "привет");
    REQUIRE_EQ(str::utf8_truncate("abc", 0), "");
    REQUIRE_EQ(str::utf8_slice_columns("abcdef", 2, 3), "cde");
    REQUIRE_EQ(str::utf8_slice_columns("日本語", 2, 2), "本");

    // Возвращается байтовое смещение в исходной строке (для подсветки в файле).
    const auto found = str::find_nocase("Путь к Файлу", "файлу");
    REQUIRE(found.has_value());
    REQUIRE_EQ(*found, std::size_t{12});
    REQUIRE_EQ(str::utf8_substr("Путь к Файлу", 7, 5), "Файлу");
    REQUIRE(!str::find_nocase("привет", "пока").has_value());

    REQUIRE_CONTAINS(str::highlight_position("abcXYZdef", 3), "▲");
}

// ===========================================================================
//  Раздел 4: ANSI
// ===========================================================================
TEST(ansi, colors_and_stripping) {
    const std::string colored = str::red_text("ошибка");
    REQUIRE_CONTAINS(colored, "\x1b[31m");
    REQUIRE(str::has_ansi(colored));
    REQUIRE_EQ(str::strip_ansi(colored), "ошибка");
    REQUIRE_EQ(str::strip_ansi("обычный"), "обычный");
    REQUIRE_EQ(str::terminal_width(colored), 6);
    REQUIRE_EQ(str::terminal_width("日本語"), 6);

    const std::string nested = str::bold_text(str::green_text("ок"));
    REQUIRE_EQ(str::terminal_width(nested), 2);
    REQUIRE_EQ(str::strip_ansi(nested), "ок");
}

TEST(ansi, truncate_visible_keeps_codes) {
    const std::string long_line = str::red_text("очень длинная строка текста");
    const std::string cut = str::truncate_visible(long_line, 10);
    REQUIRE_EQ(str::terminal_width(cut), 10);
    REQUIRE_CONTAINS(cut, "…");
    REQUIRE(str::has_ansi(cut));

    const std::string short_line = str::truncate_visible("коротко", 40);
    REQUIRE_EQ(short_line, "коротко");
}

// ===========================================================================
//  Раздел 5: экранирование и кодирование
// ===========================================================================
TEST(escaping, c_json_html) {
    REQUIRE_EQ(str::escape_c("a\nb\t\"c\""), "a\\nb\\t\\\"c\\\"");
    REQUIRE_EQ(str::unescape_c("a\\nb\\x41"), "a\nbA");
    REQUIRE_EQ(str::unescape_c("\\u041F\\u0440"), "Пр");
    REQUIRE_EQ(str::unescape_c("\\uD83D\\uDE80"), "🚀");          // суррогатная пара
    REQUIRE_EQ(str::escape_json("строка \"с\" кавычками\n"), "строка \\\"с\\\" кавычками\\n");

    REQUIRE_EQ(str::escape_html("<b>&</b>"), "&lt;b&gt;&amp;&lt;/b&gt;");
    REQUIRE_EQ(str::unescape_html("&lt;p&gt;&amp;nbsp;"), "<p>&nbsp;");
    REQUIRE_EQ(str::unescape_html("&#1055;&#x440;"), "Пр");
}

TEST(escaping, url_base64_hex) {
    REQUIRE_EQ(str::url_encode("a b+c"), "a%20b%2Bc");
    REQUIRE_EQ(str::url_encode("a b", true), "a+b");
    const auto decoded = str::url_decode("a%20b%2Bc");
    REQUIRE(decoded.has_value());
    REQUIRE_EQ(*decoded, "a b+c");
    REQUIRE(!str::url_decode("плохо%2").has_value());
    REQUIRE_EQ(str::url_path_join("http://host/api/", "/v1"), "http://host/api/v1");

    REQUIRE_EQ(str::base64_encode("Man"), "TWFu");
    REQUIRE_EQ(str::base64_encode("M"), "TQ==");
    REQUIRE_EQ(str::base64_encode("Ma"), "TWE=");
    REQUIRE_EQ(str::base64_encode(""), "");
    REQUIRE_EQ(*str::base64_decode("TWFu"), "Man");
    REQUIRE_EQ(*str::base64_decode("TQ=="), "M");
    REQUIRE(!str::base64_decode("!!!!").has_value());
    REQUIRE_EQ(str::base64url_encode("привет"), "0L_RgNC40LLQtdGC");   // «/» → «_»
    REQUIRE_EQ(*str::base64url_decode("0L_RgNC40LLQtdGC"), "привет");

    REQUIRE_EQ(str::hex_encode("AB"), "4142");
    REQUIRE_EQ(*str::hex_decode("4142"), "AB");
    REQUIRE_EQ(*str::hex_decode("0x4142"), "AB");
    REQUIRE(!str::hex_decode("414").has_value());
    REQUIRE_CONTAINS(str::binary_dump("ABC"), "41 42 43");
}

TEST(escaping, csv) {
    REQUIRE_EQ(str::csv_quote("просто"), "просто");
    REQUIRE_EQ(str::csv_quote("с,запятой"), "\"с,запятой\"");
    REQUIRE_EQ(str::csv_quote("с \"кавычкой\""), "\"с \"\"кавычкой\"\"\"");

    const auto parsed = str::csv_parse_line("a,\"b,c\",\"d\"\"e\"");
    REQUIRE_EQ(parsed.size(), std::size_t{3});
    REQUIRE_EQ(parsed[1], "b,c");
    REQUIRE_EQ(parsed[2], "d\"e");

    const std::string built = str::csv_build({{"имя", "значение"}, {"a,b", "c"}});
    REQUIRE_CONTAINS(built, "\"a,b\",c");
}

TEST(escaping, shell_and_paths) {
    REQUIRE_EQ(str::shell_quote_posix("простой"), "простой");
    REQUIRE_EQ(str::shell_quote_posix("с пробелом"), "'с пробелом'");
    REQUIRE_EQ(str::shell_quote_posix("it's"), "'it'\\''s'");
    REQUIRE_EQ(str::shell_quote_windows("C:\\Program Files\\a"), "\"C:\\Program Files\\a\"");
    REQUIRE_EQ(str::shell_join_posix({"echo", "два слова"}), "echo 'два слова'");

    const auto arguments = str::split_command_line("git commit -m \"сообщение с пробелами\"");
    REQUIRE_EQ(arguments.size(), std::size_t{4});
    REQUIRE_EQ(arguments[3], "сообщение с пробелами");

    REQUIRE_EQ(str::sanitize_filename("имя/с:плохими*символами?"), "имя_с_плохими_символами_");
    REQUIRE_EQ(str::sanitize_filename("con"), "con_");       // зарезервировано в Windows
    REQUIRE_EQ(str::sanitize_filename("хвост... "), "хвост");
    REQUIRE_EQ(str::sanitize_filename(""), "file");
}

TEST(escaping, path_helpers) {
    REQUIRE_EQ(str::normalize_path("a/./b/../c"), "a/c");
    REQUIRE_EQ(str::normalize_path("src\\core\\strings.cpp"), "src/core/strings.cpp");
    REQUIRE_EQ(str::normalize_path("/a//b/"), "/a/b");
    REQUIRE_EQ(str::normalize_path("C:\\work\\проект\\..\\файл.txt"), "C:/work/файл.txt");
    REQUIRE(str::path_is_absolute("/usr/bin"));
    REQUIRE(str::path_is_absolute("C:\\Windows"));
    REQUIRE(!str::path_is_absolute("src/main.cpp"));
    REQUIRE_EQ(str::path_join("src", "core/strings.cpp"), "src/core/strings.cpp");
    REQUIRE_EQ(str::path_join("C:/work", "D:/other"), "D:/other");
    REQUIRE_EQ(str::path_filename("src/core/strings.cpp"), "strings.cpp");
    REQUIRE_EQ(str::path_extension("strings.cpp"), ".cpp");
    REQUIRE_EQ(str::path_stem("strings.cpp"), "strings");
    REQUIRE_EQ(str::path_extension("Makefile"), "");
    REQUIRE_EQ(str::path_parent("src/core/strings.cpp"), "src/core");
    REQUIRE_EQ(str::path_relative("src/core/strings.cpp", "src"), "core/strings.cpp");
    REQUIRE(str::path_is_inside("src/core/strings.cpp", "src"));
    REQUIRE(!str::path_is_inside("src2/x.cpp", "src"));

    const auto parts = str::path_parts("src/core/x.cpp");
    REQUIRE_EQ(parts.size(), std::size_t{3});
    REQUIRE_EQ(parts[0], "src");
}

// ===========================================================================
//  Раздел 6: хеши
// ===========================================================================
TEST(hashing, known_vectors) {
    REQUIRE_EQ(str::sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE_EQ(str::sha256_hex("abc"),
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    REQUIRE_EQ(str::sha256_hex(std::string(1000, 'a')),
               "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    // Хеш кириллицы: проверяем длину и стабильность, а не «магическое» значение.
    const std::string cyrillic_hash = str::sha256_hex("привет");
    REQUIRE_EQ(cyrillic_hash.size(), std::size_t{64});
    REQUIRE_EQ(cyrillic_hash, str::sha256_hex("привет"));
    REQUIRE_NE(cyrillic_hash, str::sha256_hex("Привет"));
    REQUIRE_EQ(str::sha256_hex("a").size(), std::size_t{64});

    REQUIRE_EQ(str::crc32("123456789"), 0xCBF43926U);
    REQUIRE_EQ(str::fnv1a64(""), 14695981039346656037ULL);
    REQUIRE_EQ(str::fnv1a64("a"), 0xAF63DC4C8601EC8CULL);
    REQUIRE_EQ(str::short_hash("abc", 8), "ba7816bf");
    REQUIRE_EQ(str::stable_hash("abc"), str::fnv1a64("abc"));
}

TEST(hashing, random_tokens) {
    const std::string token1 = str::random_token(16);
    const std::string token2 = str::random_token(16);
    REQUIRE_EQ(token1.size(), std::size_t{32});
    REQUIRE_NE(token1, token2);
    REQUIRE_EQ(str::random_hex(4).size(), std::size_t{8});
    REQUIRE_EQ(str::random_token(8, false).size(), std::size_t{11});   // base64url без выравнивания
}

// ===========================================================================
//  Раздел 7: сравнение и поиск
// ===========================================================================
TEST(compare, distances) {
    REQUIRE_EQ(str::levenshtein("kitten", "sitting"), std::size_t{3});
    REQUIRE_EQ(str::levenshtein("", "abc"), std::size_t{3});
    REQUIRE_EQ(str::levenshtein("abc", "abc"), std::size_t{0});
    REQUIRE_EQ(str::levenshtein("привет", "превет"), std::size_t{1});
    REQUIRE_EQ(str::levenshtein("abcdef", "xyz", 2), std::size_t{3});   // с лимитом — «не больше»

    REQUIRE_EQ(str::damerau_levenshtein("ab", "ba"), std::size_t{1});   // перестановка
    REQUIRE_EQ(str::damerau_levenshtein("привет", "пирвет"), std::size_t{1});

    REQUIRE_NEAR(str::jaro("martha", "marhta"), 0.944, 0.01);
    REQUIRE_NEAR(str::jaro_winkler("martha", "marhta"), 0.961, 0.01);
    REQUIRE_NEAR(str::jaro("abc", "abc"), 1.0, 1e-9);
    REQUIRE_NEAR(str::jaro("", ""), 1.0, 1e-9);

    REQUIRE_EQ(str::lcs_length("AGGTAB", "GXTXAYB"), std::size_t{4});
    REQUIRE_NEAR(str::similarity_ratio("abc", "abc"), 1.0, 1e-9);
    REQUIRE(str::similarity_ratio("abcdef", "xyz") < 0.3);
}

TEST(compare, fuzzy_and_matching) {
    // Точное совпадение ценнее всего.
    REQUIRE(str::fuzzy_score("main", "main") > str::fuzzy_score("main", "main.cpp"));
    // Совпадение с начала — лучше, чем в середине.
    REQUIRE(str::fuzzy_score("read", "read_file") > str::fuzzy_score("read", "file_read"));
    // Подпоследовательность находит «cfg» → «config».
    REQUIRE(str::fuzzy_score("cfg", "config") > 0);
    REQUIRE(str::fuzzy_score("zzz", "config") < 0);

    const auto positions = str::fuzzy_positions("cfg", "config");
    REQUIRE_EQ(positions.size(), std::size_t{3});
    REQUIRE_EQ(str::fuzzy_positions("on", "config").size(), std::size_t{2});

    const auto best = str::best_match("strng", {"strings.cpp", "json.cpp", "log.cpp"});
    REQUIRE(best.has_value());
    REQUIRE_EQ(*best, "strings.cpp");

    const std::vector<std::string> candidates = {"write_file", "read_file", "delete_file"};
    const auto ranked = str::rank_matches("read", candidates, 3);
    REQUIRE_EQ(ranked.size(), std::size_t{1});      // «read» есть только в read_file
    REQUIRE_EQ(ranked[0].first, "read_file");

    const auto wider = str::rank_matches("fil", candidates, 3);
    REQUIRE_EQ(wider.size(), std::size_t{3});       // «fil» — подпоследовательность для всех
    REQUIRE_EQ(wider[0].first, "read_file");
}

TEST(compare, natural_sort_and_wildcards) {
    REQUIRE(str::natural_compare("file2", "file10") < 0);
    REQUIRE(str::natural_compare("File2", "file2") == 0);
    REQUIRE(str::natural_compare("a", "a") == 0);
    REQUIRE(str::natural_less("v1.9.0", "v1.10.0"));

    REQUIRE(str::wildcard_match("*.cpp", "main.cpp"));
    REQUIRE(!str::wildcard_match("*.cpp", "main.hpp"));
    REQUIRE(str::wildcard_match("src/*.cpp", "src/main.cpp"));
    REQUIRE(!str::wildcard_match("src/*.cpp", "src/core/main.cpp"));
    REQUIRE(str::wildcard_match("src/**/*.cpp", "src/core/main.cpp"));
    REQUIRE(str::wildcard_match("**/*.md", "docs/ARCHITECTURE.md"));
    REQUIRE(str::wildcard_match("test_?.cpp", "test_a.cpp"));
    REQUIRE(!str::wildcard_match("test_?.cpp", "test_ab.cpp"));
    REQUIRE(str::wildcard_match("[tm]ain.cpp", "main.cpp"));
    REQUIRE(str::wildcard_match("[!x]ain.cpp", "main.cpp"));
    REQUIRE(str::wildcard_match("MAIN.c*", "main.cpp", false));   // без учёта регистра

    const std::string regex = str::glob_to_regex("src/**/*.cpp");
    REQUIRE_CONTAINS(regex, "[^/\\\\]*");
}

// ===========================================================================
//  Раздел 8: текст, таблицы, разметка
// ===========================================================================
TEST(text, wrap_and_clip) {
    const std::string wrapped = str::wrap("раз два три четыре пять шесть", 10);
    for (auto line : str::line_views(wrapped, true)) {
        REQUIRE(str::display_width(line) <= 10);
    }
    REQUIRE_CONTAINS(wrapped, "\n");

    const std::string long_word = str::wrap("оченьдлинноесловобезпробелов", 6);
    REQUIRE_CONTAINS(long_word, "\n");

    const std::string block = str::wrap_block("строка один\n    отступ не трогаем\n\nконец", 20);
    REQUIRE_CONTAINS(block, "    отступ не трогаем");

    REQUIRE_EQ(str::clip("коротко", 20), "коротко");
    REQUIRE_EQ(str::clip("длинный текст", 8), "длинный…");   // 7 колонок + многоточие
    REQUIRE_CONTAINS(str::clip_middle("0123456789", 6), "…");
    REQUIRE_EQ(str::clip_lines("a\nb\nc\nd", 2).substr(0, 4), "a\nb\n");
    REQUIRE_CONTAINS(str::numbered_lines("x\ny"), "1 │ x");
    REQUIRE_CONTAINS(str::numbered_lines("x\ny", 10), "10 │ x");
}

TEST(text, tables_boxes_progress) {
    const std::string rendered = str::table({{"a", "1"}, {"длинное", "2"}}, {"имя", "n"});
    REQUIRE_CONTAINS(rendered, "имя");
    REQUIRE_CONTAINS(rendered, "┌");
    REQUIRE_CONTAINS(rendered, "длинное");

    const std::string narrow = str::table({{"очень длинная колонка", "вторая"}}, {}, 30);
    for (auto line : str::line_views(narrow, true)) {
        REQUIRE(str::terminal_width(line) <= 30);
    }

    const std::string framed = str::box("Заголовок", "строка один\nстрока два");
    REQUIRE_CONTAINS(framed, "┌─ Заголовок");
    REQUIRE_CONTAINS(framed, "строка один");

    REQUIRE_EQ(str::progress_bar(0.0, 4, false), "[░░░░]");
    REQUIRE_EQ(str::progress_bar(1.0, 4, false), "[████]");
    REQUIRE_CONTAINS(str::progress_bar(0.5, 10, true), "50%");
    REQUIRE_EQ(str::spinner_frame(0), "⠋");
    REQUIRE_EQ(str::spinner_frame(10), "⠋");     // цикл замыкается
}

TEST(text, markdown_and_templates) {
    const std::string rendered = str::markdown_to_ansi("# Заголовок\n**жирный** и `код`");
    REQUIRE_CONTAINS(rendered, "\x1b[1m");
    REQUIRE_CONTAINS(rendered, "\x1b[36m");
    REQUIRE_EQ(str::strip_ansi(rendered).find('#'), std::string::npos);
    REQUIRE_EQ(str::strip_markdown("**просто** текст").find('*'), std::string::npos);

    const std::string highlighted = str::highlight("найди файл", {"файл"});
    REQUIRE_CONTAINS(highlighted, "\x1b[");
    REQUIRE_CONTAINS(str::strip_ansi(highlighted), "найди файл");

    const auto vars = str::template_variables("{{имя}} и {{путь}}");
    REQUIRE_EQ(vars.size(), std::size_t{2});
    REQUIRE_EQ(str::render_template("{{имя}}!", {{"имя", "мир"}}), "мир!");
    REQUIRE_EQ(str::render_template("{{нет}}", {}), "{{нет}}");
}

TEST(text, stats_and_language) {
    const str::TextStats stats = str::text_stats("первая строка\nвторая строка подлиннее\n");
    REQUIRE_EQ(stats.lines, std::size_t{3});
    REQUIRE(stats.words >= 5);
    REQUIRE_EQ(stats.has_crlf, false);
    REQUIRE_EQ(str::text_stats("a\r\nb").has_crlf, true);
    REQUIRE_CONTAINS(str::text_stats_report("текст"), "Оценка токенов");
    REQUIRE(str::estimate_tokens("hello world") > 0);
    REQUIRE(str::estimate_tokens("привет мир") > str::estimate_tokens("hello world"));

    const auto sentences = str::sentences("Первое. Второе! Третье?");
    REQUIRE_EQ(sentences.size(), std::size_t{3});
    REQUIRE_EQ(sentences[0], "Первое.");
    REQUIRE_EQ(str::words("раз, два и три!"), (std::vector<std::string>{"раз", "два", "и", "три"}));

    const auto frequencies = str::word_frequencies("код код тест");
    REQUIRE_EQ(frequencies[0].first, "код");
    REQUIRE_EQ(frequencies[0].second, 2);

    const auto terms = str::key_terms("векторный индекс строит векторный индекс", 5);
    REQUIRE(!terms.empty());
    REQUIRE_CONTAINS(terms[0].first, "векторный");

    const std::string summary = str::keyword_summary(
        "Индекс строится по векторам. Строки не нужны. Векторный индекс быстрый.", 5, 2);
    REQUIRE_CONTAINS(summary, "Ключевые термины");

    REQUIRE(str::looks_like_code("#include <string>\nint main() { return 0; }"));
    REQUIRE(!str::looks_like_code("Это обычный текст про отпуск и море."));
    REQUIRE_EQ(str::detect_language_hint("src/main.cpp"), "cpp");
    REQUIRE_EQ(str::detect_language_hint("script.py"), "python");
    REQUIRE_EQ(str::detect_language_hint("noext"), "text");
}

// ===========================================================================
//  Раздел 9: diff
// ===========================================================================
TEST(diff, basic_operations) {
    const auto ops = str::diff_lines("a\nb\nc", "a\nx\nc");
    const auto stats = str::diff_stats(ops);
    REQUIRE_EQ(stats.added, std::size_t{1});
    REQUIRE_EQ(stats.removed, std::size_t{1});
    REQUIRE_EQ(stats.unchanged, std::size_t{2});
    REQUIRE(!stats.identical());
    REQUIRE_CONTAINS(stats.summary(), "+1");

    const auto identical = str::diff_stats(str::diff_lines("same\ntext", "same\ntext"));
    REQUIRE(identical.identical());

    REQUIRE_EQ(str::diff_type_marker(str::DiffType::Insert), '+');
    REQUIRE_EQ(str::diff_type_marker(str::DiffType::Delete), '-');
    REQUIRE_EQ(str::diff_type_name(str::DiffType::Equal), "равно");
}

TEST(diff, characters_and_words) {
    const auto char_ops = str::diff_chars("кот", "кит");
    const auto char_stats = str::diff_stats(char_ops);
    REQUIRE_EQ(char_stats.added, std::size_t{1});
    REQUIRE_EQ(char_stats.removed, std::size_t{1});

    const auto word_ops = str::diff_words("быстрый коричневый лис", "быстрый рыжий лис");
    const auto word_stats = str::diff_stats(word_ops);
    REQUIRE_EQ(word_stats.added, std::size_t{1});
    REQUIRE_EQ(word_stats.removed, std::size_t{1});
}

TEST(diff, unified_output_and_apply) {
    const std::string before = "one\ntwo\nthree\nfour\nfive\n";
    const std::string after = "one\ntwo\nTHREE\nfour\nfive\nsix\n";
    const std::string patch = str::unified_diff(before, after, "a.txt", "b.txt");
    REQUIRE_CONTAINS(patch, "--- a.txt");
    REQUIRE_CONTAINS(patch, "+++ b.txt");
    REQUIRE_CONTAINS(patch, "@@");
    REQUIRE_CONTAINS(patch, "-three");
    REQUIRE_CONTAINS(patch, "+THREE");
    REQUIRE_CONTAINS(patch, "+six");

    std::string error;
    const auto applied = str::apply_unified_diff(before, patch, &error);
    REQUIRE(applied.has_value());
    REQUIRE_EQ(*applied, after);
    REQUIRE(error.empty());

    // Обратное применение: патч не должен применяться к уже изменённому тексту.
    const std::string bad = str::unified_diff("a\nb\n", "a\nc\n", "x", "y");
    std::string bad_error;
    REQUIRE(!str::apply_unified_diff("совсем\nдругой\nтекст\n", bad, &bad_error).has_value());
    REQUIRE(!bad_error.empty());
    REQUIRE(!str::apply_unified_diff("текст", "без хунков", &bad_error).has_value());
}

TEST(diff, search_replace_and_similarity) {
    const std::string source = "line one\nline two\nline three\n";
    std::string error;

    const auto exact = str::apply_search_replace(source, "line two", "LINE TWO", &error);
    REQUIRE(exact.has_value());
    REQUIRE_CONTAINS(*exact, "LINE TWO");

    // Мягкий режим: модель прислала блок без внешнего отступа — возвращаем его на место.
    const std::string indented = "class A:\n    def f(self):\n        return 1\n";
    const auto soft = str::apply_search_replace(indented, "def f(self):\n    return 1",
                                                "def f(self):\n    return 2", &error);
    REQUIRE(soft.has_value());
    REQUIRE_CONTAINS(*soft, "        return 2");      // отступ 8 пробелов сохранён
    REQUIRE_CONTAINS(*soft, "    def f(self):");

    REQUIRE(!str::apply_search_replace(source, "нет такой строки", "x", &error).has_value());
    REQUIRE_CONTAINS(error, "не найден");

    REQUIRE(str::fuzzy_equal("  a b ", "a   b"));
    REQUIRE(!str::fuzzy_equal("a b", "a c"));
    REQUIRE_EQ(str::find_block(source, "line three"), std::size_t{18});
    REQUIRE_EQ(str::find_block(source, "нет"), std::string::npos);
    REQUIRE_CONTAINS(str::similarity_report("a\nb\n", "a\nc\n"), "Строк добавлено");
    REQUIRE_CONTAINS(str::similarity_report("a\nb\n", "a\nb\n"), "идентичны");
}

TEST(diff, large_inputs_are_bounded) {
    // Предохранитель: очень большие входы не должны «вешать» diff.
    std::string left;
    std::string right;
    for (int i = 0; i < 400; ++i) {
        left += "строка " + std::to_string(i) + "\n";
        right += "строка " + std::to_string(i + (i % 7 == 0 ? 1 : 0)) + "\n";
    }
    const auto ops = str::diff_lines(left, right);
    REQUIRE(!ops.empty());
    const auto stats = str::diff_stats(ops);
    REQUIRE(stats.total() > 0);
}

TEST(diff, side_by_side_and_colors) {
    const std::string rendered = str::side_by_side_diff("a\nb\n", "a\nc\n", 40);
    REQUIRE_CONTAINS(str::strip_ansi(rendered), "старое");
    REQUIRE_CONTAINS(str::strip_ansi(rendered), "новое");

    const std::string colored = str::colorize_diff(str::unified_diff("a\n", "b\n"));
    REQUIRE(str::has_ansi(colored));
    REQUIRE_CONTAINS(str::strip_ansi(colored), "-a");
}

AIA_TEST_MAIN
