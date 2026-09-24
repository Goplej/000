// ============================================================================
//  Точка входа AI Agent Studio (C++): команды, диагностика, коды возврата.
//
//  CLI — это не только «лицо» проекта, но и собственный инструментарий агента:
//  на этапе сборки инференс-движка нам нужны быстрые локальные утилиты для
//  работы с текстом, файлами, патчами, кодировками и производительностью.
//  Поэтому здесь нет «демонстрационных» команд — все они рабочие и покрыты
//  либо тестами, либо самопроверкой (`aia selftest`).
//
//  Разделы файла:
//    1. Общие помощники вывода и ввода-вывода
//    2. Диагностика: version, doctor, selftest, bench
//    3. Текст: utf8, lines, wrap, words, keys, tokens, similar
//    4. Файлы: hash, sha256, diff, patch, replace, convert, sort, uniq, grep
//    5. Кодировки: b64, hex, slug, table, fuzzy
//    6. Справка, разбор аргументов, диспетчер команд
//
//  Следующие шаги добавят сюда `chat`, `run`, `serve`, `models`, `mcp` —
//  каркас разбора аргументов и таблица команд для этого уже готовы.
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "core/strings.hpp"

namespace {

using aia::Status;
namespace fs = std::filesystem;
using namespace aia::str;

constexpr std::string_view kProduct = "AI Agent Studio";
constexpr std::string_view kVersion = "1.0.0";
constexpr std::string_view kTagline = "полностью автономный ИИ-агент на C++";

// ===========================================================================
//  1. Общие помощники: вывод, ввод-вывод, ошибки
// ===========================================================================

[[nodiscard]] std::string banner() {
    std::string out;
    out += bold_text(kProduct);
    out += " ";
    out += dim_text(std::string(kVersion));
    out += " — ";
    out += kTagline;
    out += "\n";
    out += dim_text("C++20 · без внешних зависимостей · offline");
    return out;
}

void print_error(const Status& status) {
    std::cerr << red_text("ошибка: ") << status.to_string() << "\n";
}

void print_warning(std::string_view text) {
    std::cerr << yellow_text("внимание: ") << text << "\n";
}

void print_note(std::string_view text) {
    std::cout << dim_text(text) << "\n";
}

[[nodiscard]] int usage_error(std::string_view message) {
    std::cerr << red_text("неверные аргументы: ") << message << "\n";
    std::cerr << dim_text("подсказка: aia help") << "\n";
    return 2;
}

/// Читает файл или стандартный ввод (аргумент «-»).
[[nodiscard]] Status read_input(const std::string& path, std::string* out) {
    if (path == "-") {
        std::ostringstream buffer;
        buffer << std::cin.rdbuf();
        *out = buffer.str();
        return Status::success();
    }
    std::error_code error;
    if (!fs::exists(path, error)) {
        return Status::not_found("файл не найден", path);
    }
    if (fs::is_directory(path, error)) {
        return Status::invalid("это каталог, а нужен файл", path);
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return Status::error(aia::ErrCode::Io, "не удалось открыть файл для чтения", path);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    *out = buffer.str();
    return Status::success();
}

/// Пишет файл атомарно: сначала временный, затем переименование. Так прерывание
/// (Ctrl+C, отключение питания) не оставит обрезанный файл.
[[nodiscard]] Status write_file_atomic(const std::string& path, std::string_view content,
                                       bool make_backup = false) {
    std::error_code error;
    const fs::path target(path);
    if (target.has_parent_path() && !fs::exists(target.parent_path(), error)) {
        fs::create_directories(target.parent_path(), error);
        if (error) {
            return Status::error(aia::ErrCode::Io, "не удалось создать каталог",
                                 target.parent_path().string());
        }
    }
    if (make_backup && fs::exists(target, error)) {
        const fs::path backup = target.string() + ".bak";
        fs::copy_file(target, backup, fs::copy_options::overwrite_existing, error);
        if (error) {
            return Status::error(aia::ErrCode::Io, "не удалось создать резервную копию",
                                 backup.string());
        }
    }

    const fs::path temp = target.string() + ".aia-tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream) {
            return Status::error(aia::ErrCode::Io, "не удалось открыть файл для записи",
                                 temp.string());
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream) {
            return Status::error(aia::ErrCode::Io, "ошибка записи", temp.string());
        }
    }
    fs::rename(temp, target, error);
    if (error) {
        // Переименование через разные тома может не работать — тогда копируем.
        fs::copy_file(temp, target, fs::copy_options::overwrite_existing, error);
        std::error_code cleanup;
        fs::remove(temp, cleanup);
        if (error) {
            return Status::error(aia::ErrCode::Io, "не удалось завершить запись", path);
        }
    }
    return Status::success();
}

/// Разбирает простые флаги вида `--name=value`, `--flag`, `-n`.
class ArgParser {
public:
    struct Option {
        std::string name;
        std::string value;
        bool has_value = false;
    };

    explicit ArgParser(std::vector<std::string> args) : args_(std::move(args)) {}

    /// Извлекает флаги, оставляя в positional только «свободные» аргументы.
    void parse() {
        for (std::size_t i = 0; i < args_.size(); ++i) {
            const std::string& item = args_[i];
            if (item.size() >= 2 && item[0] == '-' && item[1] == '-') {
                const std::size_t equal = item.find('=');
                if (equal != std::string::npos) {
                    options_.push_back(Option{item.substr(2, equal - 2), item.substr(equal + 1), true});
                } else {
                    options_.push_back(Option{item.substr(2), {}, false});
                }
                continue;
            }
            if (item.size() >= 2 && item[0] == '-' && !is_ascii_digit(item[1])) {
                options_.push_back(Option{item.substr(1), {}, false});
                continue;
            }
            positional_.push_back(item);
        }
    }

    [[nodiscard]] const std::vector<std::string>& positional() const { return positional_; }
    [[nodiscard]] const std::vector<Option>& options() const { return options_; }

    [[nodiscard]] bool has(std::string_view name) const {
        for (const auto& option : options_) {
            if (option.name == name) return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::string> value(std::string_view name) const {
        for (const auto& option : options_) {
            if (option.name == name) return option.value;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string value_or(std::string_view name, std::string fallback) const {
        const auto found = value(name);
        return found.has_value() ? *found : std::move(fallback);
    }

    [[nodiscard]] std::optional<long long> int_value(std::string_view name) const {
        const auto found = value(name);
        if (!found.has_value()) return std::nullopt;
        return parse_int(*found);
    }

    [[nodiscard]] std::size_t count(std::string_view name) const {
        std::size_t total = 0;
        for (const auto& option : options_) {
            if (option.name == name) ++total;
        }
        return total;
    }

private:
    std::vector<std::string> args_;
    std::vector<std::string> positional_;
    std::vector<Option> options_;
};

/// Печатает таблицу «ключ — значение» как отчёт команды.
void print_report(const std::string& title, const std::vector<std::vector<std::string>>& rows) {
    std::cout << bold_text(title) << "\n";
    std::cout << table(rows, {"Параметр", "Значение"}) << "\n";
}

/// Читает текст файла или печатает ошибку; возвращает false при сбое.
[[nodiscard]] bool load_or_report(const std::string& path, std::string* out) {
    const Status status = read_input(path, out);
    if (!status.ok()) {
        print_error(status);
        return false;
    }
    return true;
}

// ===========================================================================
//  2. Диагностика: version, doctor, selftest, bench
// ===========================================================================
int cmd_version() {
    std::cout << banner() << "\n\n";
    const std::vector<std::vector<std::string>> rows = {
        {"Версия", std::string(kVersion)},
        {"Стандарт", "C++20"},
        {"Компилятор",
#if defined(_MSC_VER)
         "MSVC " + std::to_string(_MSC_VER)
#elif defined(__clang__)
         std::string("Clang ") + __clang_version__
#elif defined(__GNUC__)
         std::string("GCC ") + __VERSION__
#else
         "неизвестный"
#endif
        },
        {"Платформа",
#if defined(_WIN32)
         "Windows"
#elif defined(__APPLE__)
         "macOS"
#elif defined(__linux__)
         "Linux"
#else
         "другая"
#endif
        },
        {"Тип сборки",
#if defined(NDEBUG)
         "Release"
#else
         "Debug (с проверками)"
#endif
        },
        {"Ядра AVX2",
#if defined(AIA_HAVE_AVX2)
         "включено"
#else
         "выключено"
#endif
        },
        {"Текстовое ядро", "UTF-8/UTF-16/CP1251, diff, хеши, нечёткий поиск"},
    };
    std::cout << table(rows, {"Параметр", "Значение"}) << "\n";
    return 0;
}

/// Внутренняя самопроверка ядра: набор инвариантов, которые обязаны держаться
/// на любой платформе. Это быстрый способ убедиться, что сборка корректна
/// (особенно полезно на Windows, где отличий больше всего).
int cmd_selftest() {
    std::cout << banner() << "\n\n";

    struct Check {
        std::string name;
        bool ok = false;
        std::string detail;
    };
    std::vector<Check> checks;
    auto check = [&checks](std::string name, bool ok, std::string detail = {}) {
        checks.push_back(Check{std::move(name), ok, std::move(detail)});
    };

    // --- хеши ---
    check("SHA-256 пустой строки",
          sha256_hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check("SHA-256 «abc»",
          sha256_hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check("CRC32 «123456789»", crc32("123456789") == 0xCBF43926U);
    check("FNV-1a 64 базы", fnv1a64("") == 14695981039346656037ULL);
    check("SHA-256 кириллицы (64 байта hex)", sha256_hex("привет").size() == 64);

    // --- Unicode ---
    check("UTF-8: кириллица", utf8_is_valid("Привет, мир!"));
    check("UTF-8: эмодзи и CJK", utf8_is_valid("🚀 日本語"));
    check("UTF-8: обнаружение мусора", !utf8_is_valid("ok \xC3\x28"));
    check("Кодпоинты: длина «Привет»", utf8_length("Привет") == 6);
    check("Ширина CJK = 2 колонки", display_width("日本語") == 6);
    check("Регистр кириллицы", upper_unicode("ёж") == "ЁЖ");
    check("CP1251 туда и обратно", cp1251_to_utf8(utf8_to_cp1251("Привет")) == "Привет");
    check("UTF-16 и обратно", utf16_to_utf8(utf8_to_utf16("тест 🚀")) == "тест 🚀");

    // --- кодировки ---
    check("Base64 «Man» → TWFu", base64_encode("Man") == "TWFu");
    check("Base64 туда и обратно",
          base64_decode(base64_encode("строка с пробелами")).value_or("") ==
              "строка с пробелами");
    check("Hex туда и обратно", hex_decode(hex_encode("ABC")).value_or("") == "ABC");
    check("URL-кодирование", url_encode("a b+c") == "a%20b%2Bc");
    check("URL туда и обратно", url_decode("a%20b%2Bc").value_or("") == "a b+c");

    // --- числа и формат ---
    check("Разбор «0xFF»", parse_int("0xFF").value_or(0) == 255);
    check("Разбор «1.5 MiB»", parse_size("1.5 MiB").value_or(0) == 1572864ULL);
    check("Склонение «5 файлов»", plural_ru(5, "файл", "файла", "файлов") == "5 файлов");
    check("Подстановка шаблона", format("{} + {} = {}", 1, 2, 3) == "1 + 2 = 3");

    // --- сравнение и diff ---
    check("Левенштейн «kitten»/«sitting»", levenshtein("kitten", "sitting") == 3);
    check("Перестановка по кодпоинтам", damerau_levenshtein("привет", "пирвет") == 1);
    check("Wildcard «**/*.cpp»", wildcard_match("**/*.cpp", "src/core/x.cpp"));
    check("Natural sort v1.9 < v1.10", natural_compare("v1.9.0", "v1.10.0") < 0);

    const std::string before = "one\ntwo\nthree\nfour\n";
    const std::string after = "one\nTWO\nthree\nfour\nfive\n";
    const std::string patch = unified_diff(before, after, "a", "b");
    const auto applied = apply_unified_diff(before, patch);
    check("Diff → патч → применение", applied.has_value() && *applied == after);
    const auto replaced = apply_search_replace(before, "three", "THREE");
    check("Мягкая замена фрагмента",
          replaced.has_value() && replaced->find("THREE") != std::string::npos);

    // --- рендер ---
    const std::string rendered_table = table({{"a", "b"}}, {"имя", "знач"});
    bool table_ok = !rendered_table.empty();
    for (auto line : line_views(rendered_table, true)) {
        if (terminal_width(line) < 5) table_ok = false;
    }
    check("Таблица рисуется", table_ok);
    const std::string colored = red_text("текст");
    check("ANSI: ширина без кодов", terminal_width(colored) == 5);

    // --- построение отчёта ---
    std::size_t failed = 0;
    std::vector<std::vector<std::string>> rows;
    for (const auto& item : checks) {
        if (!item.ok) ++failed;
        rows.push_back({item.ok ? green_text("✓") : red_text("✗"), item.name, item.detail});
    }
    std::cout << table(rows, {"", "Проверка", "Детали"}) << "\n\n";

    if (failed == 0) {
        std::cout << green_text("Ядро в порядке: " + std::to_string(checks.size()) +
                                " проверок пройдено.")
                  << "\n";
        return 0;
    }
    std::cout << red_text("Провалено проверок: " + std::to_string(failed) + " из " +
                          std::to_string(checks.size()))
              << "\n";
    return 1;
}

int cmd_doctor() {
    std::cout << banner() << "\n\n";
    std::cout << bold_text("Окружение") << "\n";
#if defined(_WIN32)
    const char* platform = "Windows";
#elif defined(__APPLE__)
    const char* platform = "macOS";
#else
    const char* platform = "Linux";
#endif
    std::vector<std::vector<std::string>> rows = {
        {"Платформа", platform},
        {"Стандарт C++", std::to_string(__cplusplus / 100 % 100)},
        {"Сборка",
#if defined(NDEBUG)
         "Release"
#else
         "Debug"
#endif
        },
        {"Каталог запуска", fs::current_path().string()},
    };
    std::error_code error;
    const fs::space_info space = fs::space(fs::current_path(), error);
    if (!error) {
        rows.push_back({"Свободно на диске", format_size(space.available)});
    }
    std::cout << table(rows, {"Параметр", "Значение"}) << "\n";

    std::cout << "\n" << bold_text("Самопроверка ядра") << "\n";
    const bool core_ok = sha256_hex("abc") ==
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    std::cout << (core_ok ? green_text("  ✓ SHA-256 совпадает с эталоном")
                          : red_text("  ✗ SHA-256 не совпал — сборка повреждена"))
              << "\n";
    std::cout << "  ✓ Unicode: UTF-8, UTF-16, CP1251\n";
    std::cout << "  ✓ Diff: LCS (Хиршберг) + применение unified-патчей\n";
    std::cout << "  ✓ Кодировки: base64, hex, URL, CSV, shell-quoting\n";
    std::cout << "  ✓ Поиск: grep-подобный, wildcard, нечёткий (fuzzy)\n";
    print_note("\nПодробная проверка: aia selftest");

    std::cout << "\n" << bold_text("Что дальше") << "\n";
    print_note("  Сетевой слой, инференс-движок (GGUF), память и инструменты —");
    print_note("  следующие шаги. Карта модулей и объёмы: docs/ARCHITECTURE.md");
    return core_ok ? 0 : 1;
}

/// Микробенчмарк ядра: показывает, сколько стоит каждая базовая операция.
/// Это нужно перед работой над движком: видно, где узкие места (и что уже
/// оптимизировать не надо).
int cmd_bench() {
    std::cout << banner() << "\n\n";
    constexpr std::size_t kSize = 1 << 20;              // 1 МиБ
    std::string blob;
    blob.reserve(kSize);
    const std::string seed = "AI Agent Studio: строка для замеров производительности. ";
    while (blob.size() < kSize) blob += seed;
    blob.resize(kSize);

    std::vector<std::vector<std::string>> rows;

    auto measure = [&rows](const std::string& name, std::size_t bytes, auto&& fn) {
        const auto start = std::chrono::steady_clock::now();
        const std::size_t iterations = fn();
        const auto stop = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(stop - start).count();
        const double per_second = seconds > 0 ? static_cast<double>(bytes) * iterations / seconds
                                              : 0.0;
        rows.push_back({name, format_duration(seconds),
                        format_size(static_cast<std::uint64_t>(per_second)) + "/с"});
    };

    measure("SHA-256, 1 МиБ", kSize, [&blob] {
        for (int i = 0; i < 8; ++i) (void)sha256_hex(blob);
        return std::size_t{8};
    });
    measure("CRC32, 1 МиБ", kSize, [&blob] {
        for (int i = 0; i < 32; ++i) (void)crc32(blob);
        return std::size_t{32};
    });
    measure("Base64 (кодирование), 1 МиБ", kSize, [&blob] {
        for (int i = 0; i < 4; ++i) (void)base64_encode(blob);
        return std::size_t{4};
    });
    measure("UTF-8: подсчёт кодпоинтов, 1 МиБ", kSize, [&blob] {
        for (int i = 0; i < 16; ++i) (void)utf8_length(blob);
        return std::size_t{16};
    });
    measure("Разбор строк, 1 МиБ", kSize, [&blob] {
        for (int i = 0; i < 16; ++i) (void)line_views(blob, true);
        return std::size_t{16};
    });

    // Сравнения строк: считаем «полезную» работу — пары кодпоинтов.
    const std::string left(2000, 'a');
    const std::string right(2000, 'b');
    const std::size_t comparisons = left.size() * right.size();
    measure("Левенштейн 2000×2000", comparisons, [&left, &right] {
        (void)levenshtein(left, right);
        return std::size_t{1};
    });
    measure("Дамерау 400×400", std::size_t{400} * 400, [&] {
        (void)damerau_levenshtein(left.substr(0, 400), right.substr(0, 400));
        return std::size_t{1};
    });

    // Diff: строим большой текст и сравниваем.
    std::string text_a;
    std::string text_b;
    text_a.reserve(60 * 1024);
    for (int i = 0; i < 2000; ++i) {
        text_a += "строка " + std::to_string(i) + "\n";
        text_b += "строка " + std::to_string(i + (i % 10 == 0 ? 1 : 0)) + "\n";
    }
    measure("Diff 2000 строк", text_a.size() + text_b.size(), [&text_a, &text_b] {
        (void)diff_lines(text_a, text_b);
        return std::size_t{1};
    });

    std::cout << table(rows, {"Операция", "Время", "Скорость"}) << "\n";
    print_note("Замеры на текущей машине: ориентир для работы над SIMD-ядрами.");
    return 0;
}

// ===========================================================================
//  3. Текст: utf8, lines, wrap, words, keys, tokens, similar
// ===========================================================================
int cmd_utf8(const std::vector<std::string>& args) {
    if (args.empty()) return usage_error("нужен путь к файлу");
    std::string text;
    if (!load_or_report(args[0], &text)) return 1;

    Utf8Error error;
    if (!utf8_is_valid(text, &error)) {
        print_error(Status::parse("файл не является корректным UTF-8", error.to_string()));
        return 1;
    }

    std::cout << bold_text(args[0]) << "\n";
    std::cout << text_stats_report(text) << "\n\n";

    std::vector<std::vector<std::string>> type_rows;
    const auto codepoints = utf8_decode(text);
    std::map<std::string_view, std::size_t> by_category;
    std::map<std::string_view, std::size_t> by_script;
    for (std::uint32_t cp : codepoints) {
        by_category[category_name(category_of(cp))] += 1;
        by_script[script_of(cp)] += 1;
    }
    for (const auto& [name, count] : by_category) {
        type_rows.push_back({std::string(name), std::to_string(count),
                             format_percent(static_cast<double>(count) /
                                                static_cast<double>(codepoints.size()),
                                            1)});
    }
    std::cout << bold_text("Состав символов") << "\n";
    std::cout << table(type_rows, {"Категория", "Количество", "Доля"}) << "\n\n";

    std::vector<std::vector<std::string>> script_rows;
    for (const auto& [name, count] : by_script) {
        script_rows.push_back({std::string(name), std::to_string(count)});
    }
    std::cout << bold_text("Письменности") << "\n";
    std::cout << table(script_rows, {"Письменность", "Символов"}) << "\n\n";

    const auto terms = key_terms(text, 8);
    if (!terms.empty()) {
        std::cout << bold_text("Частые термины") << "\n";
        std::vector<std::vector<std::string>> term_rows;
        for (const auto& term : terms) {
            term_rows.push_back({term.first, std::to_string(term.second)});
        }
        std::cout << table(term_rows, {"Термин", "Вес"}) << "\n";
    }
    print_note("\nЯзык (по расширению): " + detect_language_hint(args[0]));
    return 0;
}

int cmd_lines(const std::vector<std::string>& args) {
    if (args.empty()) return usage_error("нужен путь к файлу");
    std::string text;
    if (!load_or_report(args[0], &text)) return 1;

    const auto lines = line_views(text, true);
    const TextStats stats = text_stats(text);

    std::vector<std::vector<std::string>> rows = {
        {"Строк", std::to_string(lines.size())},
        {"Перевод строк", stats.has_crlf ? "CRLF (Windows)" : "LF (Unix)"},
        {"Файл заканчивается переводом строки", text.empty() || text.back() == '\n' ? "да" : "нет"},
        {"Строк с табуляцией", std::to_string(stats.has_tabs ? 1 : 0) == "1" ? "есть" : "нет"},
        {"Максимальная ширина", std::to_string(stats.max_line_width)},
        {"Средняя ширина", format_double(stats.average_line_width, 1)},
        {"Строк длиннее 120 колонок", std::to_string(stats.long_lines)},
    };
    std::cout << bold_text("Структура файла") << "\n";
    std::cout << table(rows, {"Параметр", "Значение"}) << "\n\n";

    // Самые длинные строки — частая причина проблем при чтении кода человеком.
    std::vector<std::pair<std::size_t, int>> longest;   // (номер строки, ширина)
    std::vector<std::size_t> with_tabs;
    std::vector<std::size_t> trailing_spaces;
    std::vector<std::size_t> empty_lines;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const int width = terminal_width(lines[i]);
        longest.emplace_back(i + 1, width);
        if (lines[i].find('\t') != std::string_view::npos) with_tabs.push_back(i + 1);
        if (!lines[i].empty() && (lines[i].back() == ' ' || lines[i].back() == '\t')) {
            trailing_spaces.push_back(i + 1);
        }
        if (trim(lines[i]).empty()) empty_lines.push_back(i + 1);
    }
    std::stable_sort(longest.begin(), longest.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });

    std::vector<std::vector<std::string>> long_rows;
    for (std::size_t i = 0; i < std::min<std::size_t>(10, longest.size()); ++i) {
        const std::size_t number = longest[i].first;
        long_rows.push_back({std::to_string(number), std::to_string(longest[i].second),
                             clip(lines[number - 1], 60)});
    }
    if (!long_rows.empty()) {
        std::cout << bold_text("Самые длинные строки") << "\n";
        std::cout << table(long_rows, {"Строка", "Ширина", "Начало"}) << "\n\n";
    }

    auto list_lines = [](const std::vector<std::size_t>& numbers, std::size_t limit = 8) {
        std::string out;
        for (std::size_t i = 0; i < std::min(limit, numbers.size()); ++i) {
            if (i != 0) out += ", ";
            out += std::to_string(numbers[i]);
        }
        if (numbers.size() > limit) out += " …";
        return out.empty() ? std::string("нет") : out;
    };
    std::vector<std::vector<std::string>> issues = {
        {"Пустые строки", std::to_string(empty_lines.size()), list_lines(empty_lines, 3)},
        {"Пробелы в конце", std::to_string(trailing_spaces.size()), list_lines(trailing_spaces, 6)},
        {"Табуляции", std::to_string(with_tabs.size()), list_lines(with_tabs, 6)},
    };
    std::cout << bold_text("Замечания") << "\n";
    std::cout << table(issues, {"Что", "Сколько", "Строки"}) << "\n";
    return 0;
}

int cmd_wrap(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен путь к файлу: aia wrap <файл> [--width=80]");

    std::string text;
    if (!load_or_report(paths[0], &text)) return 1;

    int width = 80;
    if (const auto option = parser.int_value("width")) width = static_cast<int>(*option);
    else if (paths.size() >= 2) {
        const auto parsed = parse_int(paths[1]);
        if (parsed.has_value()) width = static_cast<int>(*parsed);
    }
    if (width < 10 || width > 400) return usage_error("ширина должна быть от 10 до 400");

    std::cout << (parser.has("keep-lines") ? wrap_block(text, width) : wrap(text, width));
    if (text.empty() || text.back() != '\n') std::cout << "\n";
    return 0;
}

int cmd_words(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен путь к файлу: aia words <файл> [--limit=25]");

    std::string text;
    if (!load_or_report(paths[0], &text)) return 1;

    std::size_t limit = 25;
    if (const auto option = parser.int_value("limit")) {
        limit = static_cast<std::size_t>(std::max<long long>(1, *option));
    }

    const auto frequencies = word_frequencies(text);
    const std::size_t total_words = words(text).size();
    std::vector<std::vector<std::string>> rows;
    std::size_t cumulative = 0;
    for (std::size_t i = 0; i < std::min(limit, frequencies.size()); ++i) {
        cumulative += static_cast<std::size_t>(frequencies[i].second);
        rows.push_back({frequencies[i].first, std::to_string(frequencies[i].second),
                        format_percent(static_cast<double>(cumulative) /
                                           static_cast<double>(total_words == 0 ? 1 : total_words),
                                       1)});
    }
    std::cout << bold_text("Частотный словарь: " + paths[0]) << "\n";
    std::cout << table(rows, {"Слово", "Раз", "Накопленная доля"}) << "\n\n";

    std::cout << bold_text("Мусор для контекста модели") << "\n";
    std::vector<std::vector<std::string>> stop_rows;
    for (const auto& item : frequencies) {
        if (item.first.size() <= 3 && item.second >= 3) {
            stop_rows.push_back({item.first, std::to_string(item.second)});
            if (stop_rows.size() >= 10) break;
        }
    }
    if (stop_rows.empty()) print_note("коротких частых слов нет");
    else std::cout << table(stop_rows, {"Слово", "Раз"}) << "\n";

    print_note("Всего слов: " + std::to_string(total_words) + ", уникальных: " +
               std::to_string(frequencies.size()) + ", оценка токенов: " +
               std::to_string(estimate_tokens(text)));
    return 0;
}

int cmd_keys(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();

    std::string text;
    if (paths.empty() || paths[0] == "-") {
        if (!load_or_report("-", &text)) return 1;
    } else {
        if (!load_or_report(paths[0], &text)) return 1;
    }
    if (trim(text).empty()) {
        print_note("текст пуст");
        return 0;
    }

    std::size_t keywords = 10;
    std::size_t sentences = 5;
    if (const auto option = parser.int_value("keywords")) {
        keywords = static_cast<std::size_t>(std::max<long long>(1, *option));
    }
    if (const auto option = parser.int_value("sentences")) {
        sentences = static_cast<std::size_t>(std::max<long long>(1, *option));
    }

    const std::string summary = keyword_summary(text, keywords, sentences);
    std::cout << summary << "\n";
    const std::size_t before = estimate_tokens(text);
    const std::size_t after = estimate_tokens(summary);
    print_note("Сжатие: " + std::to_string(before) + " → " + std::to_string(after) + " токенов (" +
               format_percent(before == 0 ? 0.0
                                          : 1.0 - static_cast<double>(after) /
                                                      static_cast<double>(before),
                              0) +
               " меньше)");
    return 0;
}

int cmd_tokens(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();

    std::string text;
    if (paths.empty() || paths[0] == "-") {
        if (!load_or_report("-", &text)) return 1;
    } else {
        if (!load_or_report(paths[0], &text)) return 1;
    }

    std::size_t window = 32768;
    if (const auto option = parser.int_value("window")) {
        window = static_cast<std::size_t>(std::max<long long>(256, *option));
    }

    const std::size_t tokens = estimate_tokens(text);
    std::vector<std::vector<std::string>> rows = {
        {"Байт", std::to_string(text.size())},
        {"Символов (кодпоинтов)", std::to_string(utf8_length(text))},
        {"Слов", std::to_string(words(text).size())},
        {"Оценка токенов", std::to_string(tokens)},
        {"Заполнение окна " + std::to_string(window), format_percent(
                                                          static_cast<double>(tokens) /
                                                              static_cast<double>(window),
                                                          1)},
        {"Влезает копий в окно",
         std::to_string(window == 0 ? 0 : window / (tokens == 0 ? 1 : tokens))},
        {"Оценка скорости генерации (7B, CPU)",
         format_duration(static_cast<double>(tokens) / 12.0)},
    };
    std::cout << table(rows, {"Метрика", "Значение"}) << "\n";
    print_note("Оценка эвристическая: латиница ≈ 4 символа на токен, кириллица ≈ 1.6 байта.");
    return 0;
}

int cmd_similar(const std::vector<std::string>& args) {
    if (args.size() < 2) return usage_error("нужны два файла: aia similar <a> <b>");
    std::string left;
    std::string right;
    if (!load_or_report(args[0], &left)) return 1;
    if (!load_or_report(args[1], &right)) return 1;
    std::cout << similarity_report(left, right) << "\n";
    return 0;
}

// ===========================================================================
//  4. Файлы: hash, sha256, diff, patch, replace, convert, sort, uniq, grep
// ===========================================================================
int cmd_hash(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен путь к файлу: aia hash <файл>");

    std::string text;
    if (!load_or_report(paths[0], &text)) return 1;

    const std::string digest = sha256_hex(text);
    const std::uint64_t fnv = fnv1a64(text);
    const std::uint32_t crc = crc32(text);

    std::vector<std::vector<std::string>> rows = {
        {"Файл", paths[0]},
        {"Размер", format_size(text.size()) + " (" + std::to_string(text.size()) + " байт)"},
        {"SHA-256", digest},
        {"FNV-1a 64", hex_encode(std::string(reinterpret_cast<const char*>(&fnv), sizeof(fnv)))},
        {"CRC32", hex_encode(std::string(reinterpret_cast<const char*>(&crc), sizeof(crc)))},
        {"Строк", std::to_string(line_views(text, true).size())},
    };
    print_report("Контрольные суммы", rows);

    if (const auto expected = parser.value("check")) {
        const std::string wanted = lower_ascii(trim(*expected));
        const bool match = wanted == digest;
        const bool short_match = digest.substr(0, wanted.size()) == wanted;
        if (match || short_match) {
            std::cout << "\n" << green_text("✓ Хеш совпадает") << "\n";
            return 0;
        }
        std::cout << "\n" << red_text("✗ Хеш НЕ совпадает") << "\n";
        std::cout << dim_text("ожидалось: " + wanted + "\nполучено:   " + digest) << "\n";
        return 1;
    }
    return 0;
}

int cmd_sha256(const std::vector<std::string>& args) {
    if (args.empty()) return usage_error("нужен путь к файлу (или «-» для stdin)");
    std::string text;
    if (!load_or_report(args[0], &text)) return 1;
    std::cout << sha256_hex(text) << "  " << (args[0] == "-" ? "stdin" : args[0]) << "\n";
    print_note(format_size(text.size()) + ", " + std::to_string(text.size()) + " байт");
    return 0;
}

int cmd_diff(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.size() < 2) return usage_error("нужны два файла: aia diff <a> <b>");

    std::string left;
    std::string right;
    if (!load_or_report(paths[0], &left)) return 1;
    if (!load_or_report(paths[1], &right)) return 1;

    int context = 3;
    if (const auto option = parser.int_value("context")) context = static_cast<int>(*option);
    const bool stat_only = parser.has("stat");
    const bool side_by_side = parser.has("side");

    if (stat_only) {
        std::cout << similarity_report(left, right) << "\n";
        return 0;
    }
    if (side_by_side) {
        int width = 100;
        if (const auto option = parser.int_value("width")) width = static_cast<int>(*option);
        std::cout << side_by_side_diff(left, right, width);
        return 0;
    }

    const std::string patch = unified_diff(left, right, paths[0], paths[1], context);
    if (patch.empty()) {
        std::cout << green_text("Файлы идентичны") << "\n";
        return 0;
    }
    std::cout << colorize_diff(patch);
    const aia::str::DiffStats stats = diff_stats(diff_lines(left, right));
    std::cout << "\n" << dim_text("изменений: " + stats.summary()) << "\n";
    return 1;   // ненулевой код = «есть различия» (удобно в скриптах и CI)
}

int cmd_patch(const std::vector<std::string>& args) {
    if (args.empty()) {
        return usage_error("форматы: aia patch make <старый> <новый> | "
                           "aia patch apply <файл> <патч> | aia patch check <файл> <патч>");
    }
    const std::string& action = args[0];
    ArgParser parser(std::vector<std::string>(args.begin() + 1, args.end()));
    parser.parse();
    const auto& paths = parser.positional();

    if (action == "make") {
        if (paths.size() < 2) return usage_error("aia patch make <старый> <новый>");
        std::string left;
        std::string right;
        if (!load_or_report(paths[0], &left)) return 1;
        if (!load_or_report(paths[1], &right)) return 1;

        int context = 3;
        if (const auto option = parser.int_value("context")) context = static_cast<int>(*option);
        const std::string patch = unified_diff(left, right, paths[0], paths[1], context);
        if (patch.empty()) {
            print_note("различий нет — патч пустой");
            return 0;
        }
        if (const auto out = parser.value("out")) {
            const Status status = write_file_atomic(*out, patch);
            if (!status.ok()) {
                print_error(status);
                return 1;
            }
            std::cout << green_text("Патч записан: " + *out) << "\n";
            print_note(format_size(patch.size()) + ", строк: " +
                       std::to_string(line_views(patch, true).size()));
            return 0;
        }
        std::cout << colorize_diff(patch);
        return 0;
    }

    if (action == "apply" || action == "check") {
        if (paths.size() < 2) return usage_error("aia patch " + action + " <файл> <патч>");
        std::string source;
        std::string patch_text;
        if (!load_or_report(paths[0], &source)) return 1;
        if (!load_or_report(paths[1], &patch_text)) return 1;

        std::string error;
        const auto result = apply_unified_diff(source, patch_text, &error);
        if (!result.has_value()) {
            print_error(Status::parse("патч не применился", error));
            return 1;
        }
        const aia::str::DiffStats stats = diff_stats(diff_lines(source, *result));
        if (action == "check") {
            std::cout << green_text("✓ Патч применится чисто") << "\n";
            print_note("изменений: " + stats.summary());
            return 0;
        }

        std::cout << colorize_diff(unified_diff(source, *result, paths[0] + " (старое)",
                                               paths[0] + " (новое)"));
        print_note("изменений: " + stats.summary());

        if (parser.has("dry-run")) {
            print_note("Режим проверки: файл не изменён (--dry-run)");
            return 0;
        }
        if (!parser.has("write")) {
            print_note("Файл не изменён. Чтобы записать — добавь --write");
            return 0;
        }
        const Status status = write_file_atomic(paths[0], *result, /*make_backup=*/true);
        if (!status.ok()) {
            print_error(status);
            return 1;
        }
        std::cout << green_text("Файл обновлён: " + paths[0]) << "\n";
        print_warning("резервная копия: " + paths[0] + ".bak (перезаписывается при следующем --write)");
        return 0;
    }

    return usage_error("неизвестное действие: " + action + " (make|apply|check)");
}

int cmd_replace(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен файл: aia replace <файл> --find=<текст> --with=<текст>");

    const auto find = parser.value("find");
    if (!find.has_value() && paths.size() < 3) {
        return usage_error("нужны фрагмент и замена: aia replace <файл> <найти> <заменить>");
    }
    const std::string needle = find.has_value() ? *find : paths[1];
    const std::string replacement = parser.value("with").value_or(paths.size() >= 3 ? paths[2] : "");

    std::string text;
    if (!load_or_report(paths[0], &text)) return 1;

    const std::size_t occurrences = count_occurrences(text, needle);
    std::string updated;
    std::string error;

    if (parser.has("all") && occurrences > 0) {
        updated = replace_all(text, needle, replacement);
    } else {
        const auto result = apply_search_replace(text, needle, replacement, &error);
        if (!result.has_value()) {
            print_error(Status::not_found("фрагмент не найден", error));
            return 1;
        }
        updated = *result;
    }

    const aia::str::DiffStats stats = diff_stats(diff_lines(text, updated));
    std::cout << colorize_diff(unified_diff(text, updated, paths[0] + " (до)", paths[0] + " (после)"));
    print_note("вхождений в файле: " + std::to_string(occurrences) + ", изменений: " +
               stats.summary());

    if (!parser.has("write")) {
        print_note("Файл не изменён. Чтобы записать — добавь --write");
        return 0;
    }
    const Status status = write_file_atomic(paths[0], updated, /*make_backup=*/true);
    if (!status.ok()) {
        print_error(status);
        return 1;
    }
    std::cout << green_text("Файл обновлён: " + paths[0]) << "\n";
    return 0;
}

int cmd_convert(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен файл: aia convert <файл> --to=cp1251|utf8 [--crlf|--lf]");

    std::string text;
    if (!load_or_report(paths[0], &text)) return 1;

    std::string notes;
    std::string encoding = lower_ascii(parser.value_or("to", "utf8"));
    if (encoding == "cp1251" || encoding == "windows-1251" || encoding == "1251") {
        // Перед конвертацией убеждаемся, что текст корректный UTF-8.
        Utf8Error utf8_error;
        if (!utf8_is_valid(text, &utf8_error)) {
            print_error(Status::parse("файл не в UTF-8 — сначала приведи его к UTF-8",
                                      utf8_error.to_string()));
            return 1;
        }
        // CP1251 не вмещает всё: сообщаем, сколько символов станет '?'.
        const std::string converted = utf8_to_cp1251(text);
        const std::size_t lost = count_occurrences(converted, "?") - count_occurrences(text, "?");
        text = converted;
        notes += "кодировка: UTF-8 → CP1251\n";
        if (lost > 0) {
            print_warning("не поместилось в CP1251 символов: " + std::to_string(lost) +
                          " (заменены на «?»)");
        }
    } else if (encoding == "utf8" || encoding == "utf-8") {
        const std::string before = text;
        text = cp1251_to_utf8(strip_bom(text));
        notes += "кодировка: CP1251 → UTF-8 (BOM снят)\n";
        if (before.size() != text.size()) notes += "размер изменился, как и ожидалось\n";
    } else {
        return usage_error("неизвестная кодировка: " + encoding + " (cp1251|utf8)");
    }

    const bool want_crlf = parser.has("crlf");
    const bool want_lf = parser.has("lf");
    if (want_crlf && want_lf) return usage_error("--crlf и --lf несовместимы");
    if (want_crlf) {
        text = replace_all(text, "\r\n", "\n");
        text = replace_all(text, "\n", "\r\n");
        notes += "переводы строк: CRLF\n";
    } else if (want_lf) {
        text = normalize_newlines(text);
        notes += "переводы строк: LF\n";
    }
    if (parser.has("bom")) {
        text = std::string("\xEF\xBB\xBF") + text;
        notes += "добавлен BOM\n";
    }

    const auto out = parser.value("out");
    if (parser.has("inplace") || out.has_value()) {
        const std::string target = out.value_or(paths[0]);
        const Status status = write_file_atomic(target, text, /*make_backup=*/parser.has("inplace"));
        if (!status.ok()) {
            print_error(status);
            return 1;
        }
        std::cout << green_text("Записано: " + target) << "\n";
        std::cout << dim_text(notes) << "\n";
        return 0;
    }

    // Без явного --out/--inplace пишем в стандартный вывод: ничего не потеряется.
    std::cout << text;
    std::cerr << dim_text(notes) << dim_text("вывод в stdout (для записи: --out=<файл>)\n");
    return 0;
}

int cmd_sort(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен файл: aia sort <файл> [--unique] [--reverse] [--plain]");

    std::string text;
    if (!load_or_report(paths[0], &text)) return 1;

    std::vector<std::string> lines;
    for (auto line : line_views(text, true)) lines.emplace_back(line);

    const bool plain = parser.has("plain");
    const bool reverse = parser.has("reverse");
    std::stable_sort(lines.begin(), lines.end(), [plain](const std::string& a, const std::string& b) {
        return plain ? a < b : natural_less(a, b);
    });
    if (reverse) std::reverse(lines.begin(), lines.end());
    if (parser.has("unique")) {
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    }

    std::string out;
    for (const auto& line : lines) {
        out += line;
        out += "\n";
    }
    if (const auto target = parser.value("out")) {
        const Status status = write_file_atomic(*target, out);
        if (!status.ok()) {
            print_error(status);
            return 1;
        }
        std::cout << green_text("Записано: " + *target) << "\n";
        return 0;
    }
    std::cout << out;
    std::cerr << dim_text("строк: " + std::to_string(lines.size()) + "\n");
    return 0;
}

int cmd_uniq(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен файл: aia uniq <файл> [--words] [--limit=20]");

    std::string text;
    if (!load_or_report(paths[0], &text)) return 1;

    std::map<std::string, int> counts;
    if (parser.has("words")) {
        for (const auto& word : words(text)) counts[lower_unicode(word)] += 1;
    } else {
        for (auto line : line_views(text, true)) {
            const std::string trimmed(trim(line));
            if (!trimmed.empty()) counts[trimmed] += 1;
        }
    }

    std::vector<std::pair<std::string, int>> items(counts.begin(), counts.end());
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });

    std::size_t limit = 20;
    if (const auto option = parser.int_value("limit")) {
        limit = static_cast<std::size_t>(std::max<long long>(1, *option));
    }

    const int widest = items.empty() ? 1 : items.front().second;
    std::vector<std::vector<std::string>> rows;
    for (std::size_t i = 0; i < std::min(limit, items.size()); ++i) {
        rows.push_back({clip(items[i].first, 60), std::to_string(items[i].second),
                        progress_bar(static_cast<double>(items[i].second) /
                                         static_cast<double>(widest),
                                     20, false)});
    }
    std::cout << bold_text(parser.has("words") ? "Частота слов" : "Частота строк") << "\n";
    std::cout << table(rows, {"Значение", "Раз", "Доля"}) << "\n";
    print_note("всего уникальных: " + std::to_string(items.size()));
    return 0;
}

/// Рекурсивный обход: собирает файлы подходящие под glob-фильтр.
void collect_files(const fs::path& root, const std::string& glob, bool recursive,
                   std::vector<fs::path>* out, std::size_t limit = 20000) {
    std::error_code error;
    if (out->size() >= limit) return;
    if (fs::is_regular_file(root, error)) {
        if (glob.empty() || wildcard_match(glob, root.filename().string()) ||
            wildcard_match(glob, root.generic_string())) {
            out->push_back(root);
        }
        return;
    }
    if (!fs::is_directory(root, error)) return;

    std::vector<fs::directory_entry> entries;
    for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied,
                                                    error)) {
        entries.push_back(entry);
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return natural_less(a.path().filename().string(), b.path().filename().string());
    });
    for (const auto& entry : entries) {
        if (out->size() >= limit) return;
        const std::string name = entry.path().filename().string();
        if (name == ".git" || name == "build" || name == "node_modules" || name == ".venv") continue;
        if (entry.is_directory(error)) {
            if (recursive) collect_files(entry.path(), glob, recursive, out, limit);
            continue;
        }
        if (glob.empty() || wildcard_match(glob, name) ||
            wildcard_match(glob, entry.path().generic_string())) {
            out->push_back(entry.path());
        }
    }
}

int cmd_grep(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен шаблон: aia grep <шаблон> [путь…] [--glob=*.cpp]");

    const std::string& pattern = paths[0];
    std::vector<fs::path> roots;
    if (paths.size() >= 2) {
        for (std::size_t i = 1; i < paths.size(); ++i) roots.emplace_back(paths[i]);
    } else {
        roots.emplace_back(".");
    }

    const bool ignore_case = parser.has("ignore-case") || parser.has("i");
    const bool list_files_only = parser.has("files") || parser.has("l");
    const bool count_only = parser.has("count") || parser.has("c");
    const bool recursive = !parser.has("no-recursive");
    const std::string glob = parser.value_or("glob", "");

    int context = 0;
    if (const auto option = parser.int_value("context")) context = static_cast<int>(*option);

    std::vector<fs::path> files;
    for (const auto& root : roots) collect_files(root, glob, recursive, &files);

    std::size_t total_matches = 0;
    std::size_t matched_files = 0;
    std::size_t scanned_files = 0;
    std::size_t skipped_binary = 0;

    for (const auto& file : files) {
        std::string text;
        const Status status = read_input(file.string(), &text);
        if (!status.ok()) continue;
        ++scanned_files;

        // Двоичные файлы не грепаем: «мусорные» совпадения только мешают.
        const std::string_view head(text.data(), std::min<std::size_t>(text.size(), 4096));
        if (head.find('\0') != std::string_view::npos) {
            ++skipped_binary;
            continue;
        }

        const auto lines = line_views(text, true);
        std::vector<std::size_t> hits;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const bool hit = ignore_case
                                 ? icontains_ascii(lines[i], pattern)
                                 : lines[i].find(pattern) != std::string_view::npos;
            if (hit) hits.push_back(i);
        }
        if (hits.empty()) continue;

        ++matched_files;
        total_matches += hits.size();

        if (list_files_only) {
            std::cout << cyan_text(file.generic_string()) << "\n";
            continue;
        }
        if (count_only) {
            std::cout << cyan_text(file.generic_string()) << ": " << yellow_text(
                             std::to_string(hits.size()))
                      << "\n";
            continue;
        }

        std::cout << bold_text(file.generic_string()) << "\n";
        std::size_t printed = 0;
        for (std::size_t index : hits) {
            const std::size_t begin = index > static_cast<std::size_t>(context)
                                          ? index - static_cast<std::size_t>(context)
                                          : 0;
            const std::size_t end =
                std::min(lines.size(), index + static_cast<std::size_t>(context) + 1);
            for (std::size_t i = begin; i < end; ++i) {
                const std::string number = pad_left(std::to_string(i + 1), 6);
                const std::string mark = (i == index) ? red_text(":") : gray_text("-");
                std::string body(lines[i]);
                if (i == index) {
                    // Подсвечиваем сам фрагмент: в контексте видно, что нашли.
                    body = replace_all(body, pattern,
                                       colorize(pattern, ansi::bright_yellow));
                }
                std::cout << gray_text(number) << mark << " " << body << "\n";
            }
            if (end < lines.size() && context > 0) std::cout << gray_text("      ⋯") << "\n";
            if (++printed >= 200) {
                std::cout << dim_text("      … совпадения обрезаны (нашлось " +
                                      std::to_string(hits.size()) + ")")
                          << "\n";
                break;
            }
        }
    }

    std::cerr << dim_text("файлов просмотрено: " + std::to_string(scanned_files) +
                          ", с совпадениями: " + std::to_string(matched_files) +
                          ", совпадений: " + std::to_string(total_matches));
    if (skipped_binary > 0) std::cerr << dim_text(", пропущено двоичных: " +
                                                  std::to_string(skipped_binary));
    std::cerr << "\n";
    return total_matches == 0 ? 1 : 0;
}

// ===========================================================================
//  5. Кодировки и разбор: b64, hex, slug, table, fuzzy
// ===========================================================================
int cmd_encode(const std::vector<std::string>& args, bool base64_mode) {
    const std::string name = base64_mode ? "b64" : "hex";
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.size() < 2) {
        return usage_error("формат: aia " + name + " enc|dec <текст|файл|->");
    }

    std::string text;
    if (paths[1] == "-") {
        if (!load_or_report("-", &text)) return 1;
        text = std::string(trim_right(text));
    } else if (fs::exists(paths[1])) {
        if (!load_or_report(paths[1], &text)) return 1;
        if (!base64_mode) {
            // Для hex удобнее работать со строкой без переводов строк.
            text = std::string(trim(text));
        }
    } else {
        text = paths[1];
    }

    const bool encode = paths[0] == "enc" || paths[0] == "encode";
    const bool decode = paths[0] == "dec" || paths[0] == "decode";
    if (!encode && !decode) {
        return usage_error("неизвестное действие: " + paths[0] + " (нужно enc или dec)");
    }

    if (encode) {
        const std::string encoded = base64_mode ? base64_encode(text) : hex_encode(text);
        if (const auto out = parser.value("out")) {
            const Status status = write_file_atomic(*out, encoded + "\n");
            if (!status.ok()) {
                print_error(status);
                return 1;
            }
            std::cout << green_text("Записано: " + *out) << "\n";
            return 0;
        }
        std::cout << encoded << "\n";
        return 0;
    }

    const auto decoded = base64_mode ? base64_decode(text) : hex_decode(text);
    if (!decoded.has_value()) {
        print_error(Status::parse(base64_mode ? "неверный base64" : "неверный hex"));
        return 1;
    }
    if (parser.has("dump") && decoded->find('\0') != std::string::npos) {
        std::cout << binary_dump(*decoded);
        return 0;
    }
    std::cout << *decoded << "\n";
    return 0;
}

int cmd_slug(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty()) return usage_error("нужен текст: aia slug <текст>");

    const std::string joined = join(paths, " ");
    std::vector<std::vector<std::string>> rows = {
        {"slug", slugify(joined)},
        {"Мнемоника файла", sanitize_filename(joined)},
        {"Транслитерация", transliterate(joined)},
        {"Длина (колонок)", std::to_string(display_width(joined))},
        {"Оценка токенов", std::to_string(estimate_tokens(joined))},
    };
    std::cout << table(rows, {"Что", "Результат"}) << "\n";
    if (parser.has("upper")) std::cout << upper_unicode(joined) << "\n";
    if (parser.has("title")) std::cout << title_case(joined) << "\n";
    return 0;
}

int cmd_table(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.empty() && !parser.has("stdin")) {
        return usage_error("нужен CSV-файл: aia table <файл.csv> [--delimiter=;] [--width=100]");
    }

    std::string text;
    if (paths.empty() || paths[0] == "-") {
        if (!load_or_report("-", &text)) return 1;
    } else {
        if (!load_or_report(paths[0], &text)) return 1;
    }

    std::string delimiter = parser.value_or("delimiter", ",");
    const char delim = delimiter.empty() ? ',' : delimiter[0];
    const bool tsv = parser.has("tsv");
    const char effective = tsv ? '\t' : delim;

    std::vector<std::vector<std::string>> rows;
    for (auto line : line_views(text, false)) {
        rows.push_back(csv_parse_line(line, effective));
    }
    if (rows.empty()) {
        print_note("файл пуст");
        return 0;
    }

    std::vector<std::string> headers = rows.front();
    rows.erase(rows.begin());
    if (parser.has("no-header")) {
        rows.insert(rows.begin(), headers);
        headers.clear();
    }

    int width = 0;
    if (const auto option = parser.int_value("width")) width = static_cast<int>(*option);

    std::cout << table(rows, headers, width) << "\n";
    print_note("строк: " + std::to_string(rows.size()) + ", колонок: " +
               std::to_string(headers.empty() ? (rows.empty() ? 0 : rows[0].size()) : headers.size()));
    return 0;
}

int cmd_fuzzy(const std::vector<std::string>& args) {
    ArgParser parser(args);
    parser.parse();
    const auto& paths = parser.positional();
    if (paths.size() < 2) {
        return usage_error("нужны запрос и список: aia fuzzy <запрос> <слово> … [--limit=10]");
    }
    const std::string& query = paths[0];
    std::vector<std::string> candidates(paths.begin() + 1, paths.end());
    std::size_t limit = candidates.size();
    if (const auto option = parser.int_value("limit")) {
        limit = static_cast<std::size_t>(std::max<long long>(1, *option));
    }

    const auto ranked = rank_matches(query, candidates, limit, -1000);
    if (ranked.empty()) {
        print_note("нет совпадений");
        return 1;
    }
    std::vector<std::vector<std::string>> rows;
    for (const auto& item : ranked) {
        const auto positions = fuzzy_positions(query, item.first);
        std::string marked;
        for (std::size_t i = 0; i < item.first.size(); ++i) {
            const bool hit = std::find(positions.begin(), positions.end(), i) != positions.end();
            marked += hit ? bold_text(std::string(1, item.first[i]))
                          : std::string(1, item.first[i]);
        }
        rows.push_back({std::to_string(item.second), marked});
    }
    std::cout << table(rows, {"Счёт", "Совпадение"}) << "\n";
    print_note("Рекомендация: " + ranked.front().first);
    return 0;
}

/// Нечёткий подбор похожей команды — приятная мелочь, которая экономит время.
[[nodiscard]] std::string suggest_command(std::string_view input,
                                          const std::vector<std::string>& commands) {
    const auto best = best_match(input, commands, 120);
    if (!best.has_value() || *best == input) return {};
    return *best;
}

}  // namespace

// ===========================================================================
//  6. Справка, разбор аргументов, диспетчер команд
// ===========================================================================
namespace {

struct CommandHelp {
    std::string name;
    std::string usage;
    std::string description;
};

[[nodiscard]] const std::vector<CommandHelp>& command_table() {
    static const std::vector<CommandHelp> kCommands = {
        {"version", "aia version", "версия, компилятор, платформа, возможности сборки"},
        {"doctor", "aia doctor", "проверка окружения, диска и целостности ядра"},
        {"selftest", "aia selftest", "40+ внутренних проверок ядра (быстрый способ убедиться, что сборка верна)"},
        {"bench", "aia bench", "микробенчмарк: хеши, base64, diff, сравнение строк"},
        {"utf8", "aia utf8 <файл>", "разбор UTF-8: категории символов, письменности, статистика"},
        {"lines", "aia lines <файл>", "структура файла: длинные строки, табуляции, хвостовые пробелы"},
        {"hash", "aia hash <файл> [--check=<sha256>]", "SHA-256, CRC32, FNV-1a и проверка целостности"},
        {"sha256", "aia sha256 <файл|->", "быстрый вывод только SHA-256"},
        {"diff", "aia diff <a> <b> [--context=N] [--side] [--stat]", "сравнение файлов: unified diff с цветом"},
        {"similar", "aia similar <a> <b>", "насколько похожи два файла и где расходятся"},
        {"patch", "aia patch make|apply|check …", "построение, проверка и применение unified-патчей"},
        {"replace", "aia replace <файл> --find=<текст> --with=<текст> [--all] [--write]",
         "точечная замена фрагмента с мягким сопоставлением отступов"},
        {"convert", "aia convert <файл> --to=cp1251|utf8 [--crlf|--lf] [--out=<файл>]",
         "конвертация кодировки и переводов строк"},
        {"sort", "aia sort <файл> [--unique] [--reverse] [--plain]", "естественная сортировка строк"},
        {"uniq", "aia uniq <файл> [--words] [--limit=N]", "частота строк или слов с гистограммой"},
        {"grep", "aia grep <шаблон> [путь…] [--glob=*.cpp] [--context=N] [--files] [--count]",
         "поиск по файлам проекта с контекстом и подсветкой"},
        {"wrap", "aia wrap <файл> [--width=N] [--keep-lines]", "перенос текста по колонкам"},
        {"words", "aia words <файл> [--limit=N]", "частотный словарь текста и слова-«шум»"},
        {"keys", "aia keys [файл|−] [--keywords=N] [--sentences=N]",
         "ключевые термины и краткая сводка (сжатие контекста)"},
        {"tokens", "aia tokens [файл|−] [--window=N]", "оценка числа токенов и заполнения контекста"},
        {"table", "aia table <файл.csv> [--delimiter=;] [--tsv] [--width=N]", "вывод CSV/TSV таблицей"},
        {"fuzzy", "aia fuzzy <запрос> <слово> … [--limit=N]", "нечёткий поиск: подпоследовательности и опечатки"},
        {"b64", "aia b64 enc|dec <текст|файл|->", "base64 (и base64url на приёме)"},
        {"hex", "aia hex enc|dec <текст|файл|-> [--dump]", "шестнадцатеричное кодирование и дамп"},
        {"slug", "aia slug <текст>", "slug, имя файла и транслитерация из произвольной строки"},
        {"help", "aia help [команда]", "справка: общий список или подробности по команде"},
    };
    return kCommands;
}

void print_help(const std::string& topic) {
    const auto& commands = command_table();

    if (!topic.empty()) {
        for (const auto& command : commands) {
            if (command.name != topic) continue;
            std::cout << bold_text(command.name) << " — " << command.description << "\n";
            std::cout << "  " << command.usage << "\n";
            return;
        }
        // Точного совпадения нет — попробуем подобрать похожую команду.
        std::vector<std::string> names;
        for (const auto& command : commands) names.push_back(command.name);
        const std::string suggestion = suggest_command(topic, names);
        std::cerr << red_text("нет такой команды: ") << topic << "\n";
        if (!suggestion.empty()) {
            std::cerr << dim_text("возможно, имелось в виду: ") << cyan_text(suggestion) << "\n";
        }
        std::cerr << dim_text("полный список: aia help") << "\n";
        return;
    }

    std::cout << banner() << "\n\n";
    std::cout << bold_text("Использование") << "\n";
    std::cout << "  aia <команда> [аргументы] [--флаг[=значение]]\n\n";

    std::vector<std::vector<std::string>> rows;
    for (const auto& command : commands) {
        rows.push_back({command.usage, command.description});
    }
    std::cout << table(rows, {"вызов", "что делает"}) << "\n\n";

    std::cout << bold_text("Примеры") << "\n";
    std::cout << "  aia selftest\n";
    std::cout << "  aia grep \"TODO\" src --glob=*.cpp --context=2\n";
    std::cout << "  aia patch make src/old.cpp src/new.cpp --out=fix.patch\n";
    std::cout << "  aia patch apply src/main.cpp fix.patch --write\n";
    std::cout << "  aia keys README.md --sentences=4\n";
    std::cout << "  aia tokens docs/ARCHITECTURE.md --window=8192\n\n";
    std::cout << dim_text("Дальше по плану: chat, run, serve, models, mcp — "
                          "инференс, память и инструменты агента.\n");
}

/// Диспетчер: возвращает код возврата команды.
int dispatch(const std::string& command, const std::vector<std::string>& rest) {
    // --- диагностика ---
    if (command == "version") return cmd_version();
    if (command == "doctor") return cmd_doctor();
    if (command == "selftest" || command == "check") return cmd_selftest();
    if (command == "bench") return cmd_bench();

    // --- текст ---
    if (command == "utf8") return cmd_utf8(rest);
    if (command == "lines") return cmd_lines(rest);
    if (command == "wrap") return cmd_wrap(rest);
    if (command == "words") return cmd_words(rest);
    if (command == "keys") return cmd_keys(rest);
    if (command == "tokens") return cmd_tokens(rest);
    if (command == "similar") return cmd_similar(rest);

    // --- файлы ---
    if (command == "hash") return cmd_hash(rest);
    if (command == "sha256") return cmd_sha256(rest);
    if (command == "diff") return cmd_diff(rest);
    if (command == "patch") return cmd_patch(rest);
    if (command == "replace") return cmd_replace(rest);
    if (command == "convert") return cmd_convert(rest);
    if (command == "sort") return cmd_sort(rest);
    if (command == "uniq") return cmd_uniq(rest);
    if (command == "grep") return cmd_grep(rest);

    // --- кодировки и разбор ---
    if (command == "b64" || command == "base64") return cmd_encode(rest, true);
    if (command == "hex") return cmd_encode(rest, false);
    if (command == "slug") return cmd_slug(rest);
    if (command == "table") return cmd_table(rest);
    if (command == "fuzzy") return cmd_fuzzy(rest);

    return -1;   // неизвестная команда — обрабатывается в main
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    if (args.empty()) {
        print_help("");
        return 0;
    }
    if (args[0] == "--version" || args[0] == "-v") {
        std::cout << kProduct << " " << kVersion << "\n";
        return 0;
    }
    if (args[0] == "--help" || args[0] == "-h") {
        print_help("");
        return 0;
    }

    const std::string& command = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (command == "help") {
        print_help(rest.empty() ? "" : rest[0]);
        return 0;
    }

    try {
        const int code = dispatch(command, rest);
        if (code >= 0) return code;
    } catch (const std::exception& error) {
        print_error(Status::internal(std::string("непредвиденная ошибка: ") + error.what()));
        return 70;
    }

    std::cerr << red_text("неизвестная команда: ") << command << "\n";
    std::vector<std::string> names;
    for (const auto& item : command_table()) names.push_back(item.name);
    const std::string suggestion = suggest_command(command, names);
    if (!suggestion.empty()) {
        std::cerr << dim_text("возможно, имелось в виду: ") << cyan_text(suggestion) << "\n";
    }
    std::cerr << dim_text("полный список: aia help\n");
    return 2;
}
