// ============================================================================
//  Мини-фреймворк тестов: без внешних зависимостей.
//
//  Почему свой: тесты — часть проекта, а проект объявлен zero-dependency.
//  Возможностей ровно столько, сколько нужно: группы тестов, проверки с
//  понятными сообщениями, суммарный отчёт и код возврата для CTest.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdio>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aia::test {

// --- ANSI только если вывод в терминал и он это поддерживает ------------------
inline bool colors_enabled() {
#if defined(_WIN32)
    return false;   // в Windows-консоли по умолчанию не включаем, чтобы не ломать вывод
#else
    return true;
#endif
}

inline std::string_view green() { return colors_enabled() ? "\x1b[32m" : ""; }
inline std::string_view red() { return colors_enabled() ? "\x1b[31m" : ""; }
inline std::string_view yellow() { return colors_enabled() ? "\x1b[33m" : ""; }
inline std::string_view dim() { return colors_enabled() ? "\x1b[2m" : ""; }
inline std::string_view reset() { return colors_enabled() ? "\x1b[0m" : ""; }

// ---------------------------------------------------------------------------
//  Реестр тестов
// ---------------------------------------------------------------------------
class Registry {
public:
    using Body = std::function<void()>;

    struct Entry {
        std::string suite;
        std::string name;
        Body body;
    };

    static Registry& instance() {
        static Registry registry;
        return registry;
    }

    void add(std::string suite, std::string name, Body body) {
        entries_.push_back(Entry{std::move(suite), std::move(name), std::move(body)});
    }

    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }

private:
    std::vector<Entry> entries_;
};

/// Регистратор: `static TestRegistrar reg_("suite", "name", [] { ... });`
class Registrar {
public:
    Registrar(std::string suite, std::string name, Registry::Body body) {
        Registry::instance().add(std::move(suite), std::move(name), std::move(body));
    }
};

// ---------------------------------------------------------------------------
//  Провал проверки
// ---------------------------------------------------------------------------
struct AssertionFailure : std::exception {
    std::string message;
    explicit AssertionFailure(std::string text) : message(std::move(text)) {}
    [[nodiscard]] const char* what() const noexcept override { return message.c_str(); }
};

[[noreturn]] inline void fail(std::string message) { throw AssertionFailure(std::move(message)); }

// Проверяем, умеет ли тип печататься в ostream: нужно, чтобы сообщения о провале
// работали и с enum, и с vector, и с чем угодно ещё — без падений компиляции.
template <typename T, typename = void>
struct has_stream_operator : std::false_type {};

template <typename T>
struct has_stream_operator<
    T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
struct is_optional : std::false_type {};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

/// Приводит значение к строке для сообщений: строки — в кавычках, остальное как есть.
template <typename T>
std::string describe(const T& value) {
    using U = std::remove_cvref_t<T>;
    std::ostringstream stream;
    if constexpr (std::is_same_v<U, bool>) {
        stream << (value ? "true" : "false");
    } else if constexpr (std::is_enum_v<U>) {
        stream << static_cast<long long>(value);   // у enum нет оператора вывода
    } else if constexpr (std::is_convertible_v<U, std::string_view>) {
        stream << '"' << std::string_view(value) << '"';
    } else if constexpr (std::is_same_v<U, std::nullptr_t>) {
        stream << "nullptr";
    } else if constexpr (is_optional<U>::value) {
        stream << (value.has_value() ? describe(*value) : std::string("nullopt"));
    } else if constexpr (has_stream_operator<U>::value) {
        stream << value;
    } else {
        stream << "<значение без оператора вывода>";
    }
    return stream.str();
}

/// Печать всех элементов контейнера — удобно в сообщениях для vector<string>.
template <typename Container>
std::string describe_range(const Container& items) {
    std::string out = "[";
    bool first = true;
    for (const auto& item : items) {
        if (!first) out += ", ";
        first = false;
        out += describe(item);
    }
    out += "]";
    return out;
}

inline std::string escape_for_message(std::string_view text, std::size_t limit = 200) {
    std::string out;
    const std::size_t size = std::min(limit, text.size());
    for (std::size_t i = 0; i < size; ++i) {
        const char c = text[i];
        switch (c) {
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '\0': out.append("\\0"); break;
            default: out.push_back(c); break;
        }
    }
    if (text.size() > limit) out.append("…");
    return out;
}

// ---------------------------------------------------------------------------
//  Проверки
// ---------------------------------------------------------------------------
struct Failure {
    std::string message;
};

// Макросы возвращают std::string — пустая строка означает «всё хорошо».
#define AIA_CHECK_IMPL(condition, text)                       \
    [&]() -> ::std::string {                                  \
        if (!(condition)) {                                   \
            std::ostringstream oss;                           \
            oss << text;                                      \
            return oss.str();                                 \
        }                                                     \
        return {};                                            \
    }()

#define CHECK(condition) \
    AIA_CHECK_IMPL((condition), "проверка не прошла: " #condition)

#define CHECK_EQ(actual, expected)                                                       \
    [&]() -> ::std::string {                                                             \
        const auto a = (actual);                                                         \
        const auto b = (expected);                                                       \
        if (!(a == b)) {                                                                 \
            std::ostringstream oss;                                                      \
            oss << "ожидалось " << ::aia::test::describe(b) << ", получено "             \
                << ::aia::test::describe(a) << "  (" #actual " == " #expected ")";       \
            return oss.str();                                                            \
        }                                                                                \
        return {};                                                                       \
    }()

#define CHECK_NE(actual, expected)                                                       \
    [&]() -> ::std::string {                                                             \
        const auto a = (actual);                                                         \
        const auto b = (expected);                                                       \
        if (a == b) {                                                                    \
            std::ostringstream oss;                                                      \
            oss << "значения совпали, хотя не должны: " << ::aia::test::describe(a);     \
            return oss.str();                                                            \
        }                                                                                \
        return {};                                                                       \
    }()

#define CHECK_TRUE(condition) CHECK(condition)
#define CHECK_FALSE(condition) CHECK(!(condition))

#define CHECK_NEAR(actual, expected, tolerance)                                          \
    [&]() -> ::std::string {                                                             \
        const double a = static_cast<double>(actual);                                    \
        const double b = static_cast<double>(expected);                                  \
        const double t = static_cast<double>(tolerance);                                 \
        const double diff = a > b ? a - b : b - a;                                       \
        if (diff > t) {                                                                  \
            std::ostringstream oss;                                                      \
            oss << "разница " << diff << " больше допуска " << t << " (" << a << " vs "  \
                << b << ")";                                                             \
            return oss.str();                                                            \
        }                                                                                \
        return {};                                                                       \
    }()

#define CHECK_CONTAINS(text, fragment)                                                    \
    [&]() -> ::std::string {                                                              \
        const std::string haystack = (text);                                              \
        const std::string needle = (fragment);                                            \
        if (haystack.find(needle) == std::string::npos) {                                 \
            std::ostringstream oss;                                                       \
            oss << "в тексте нет «" << ::aia::test::escape_for_message(needle) << "»\\n"   \
                << "  текст: " << ::aia::test::escape_for_message(haystack);              \
            return oss.str();                                                             \
        }                                                                                \
        return {};                                                                        \
    }()

#define CHECK_NOT_CONTAINS(text, fragment)                                                \
    [&]() -> ::std::string {                                                              \
        const std::string haystack = (text);                                              \
        const std::string needle = (fragment);                                            \
        if (haystack.find(needle) != std::string::npos) {                                 \
            std::ostringstream oss;                                                       \
            oss << "в тексте неожиданно есть «" << ::aia::test::escape_for_message(needle) \
                << "»";                                                                   \
            return oss.str();                                                             \
        }                                                                                \
        return {};                                                                        \
    }()

/// Выполняет проверку и бросает AssertionFailure при провале.
#define REQUIRE(condition)                                             \
    do {                                                               \
        const std::string aia_message = CHECK(condition);              \
        if (!aia_message.empty()) ::aia::test::fail(aia_message);      \
    } while (false)

#define REQUIRE_EQ(actual, expected)                                          \
    do {                                                                      \
        const std::string aia_message = CHECK_EQ(actual, expected);           \
        if (!aia_message.empty()) ::aia::test::fail(aia_message);             \
    } while (false)

#define REQUIRE_TRUE(condition) REQUIRE(condition)

#define REQUIRE_NE(actual, expected)                                          \
    do {                                                                      \
        const std::string aia_message = CHECK_NE(actual, expected);           \
        if (!aia_message.empty()) ::aia::test::fail(aia_message);             \
    } while (false)

#define REQUIRE_FALSE(condition) REQUIRE(!(condition))

#define REQUIRE_NOT_CONTAINS(text, fragment)                                  \
    do {                                                                      \
        const std::string aia_message = CHECK_NOT_CONTAINS(text, fragment);   \
        if (!aia_message.empty()) ::aia::test::fail(aia_message);             \
    } while (false)

#define REQUIRE_NEAR(actual, expected, tolerance)                             \
    do {                                                                      \
        const std::string aia_message = CHECK_NEAR(actual, expected, tolerance); \
        if (!aia_message.empty()) ::aia::test::fail(aia_message);             \
    } while (false)

#define REQUIRE_CONTAINS(text, fragment)                                      \
    do {                                                                      \
        const std::string aia_message = CHECK_CONTAINS(text, fragment);       \
        if (!aia_message.empty()) ::aia::test::fail(aia_message);             \
    } while (false)

// ---------------------------------------------------------------------------
//  Объявление тестов
// ---------------------------------------------------------------------------
#define TEST(suite_name, test_name)                                              \
    static void suite_name##_##test_name##_body();                               \
    static ::aia::test::Registrar suite_name##_##test_name##_registrar(          \
        #suite_name, #test_name, suite_name##_##test_name##_body);               \
    static void suite_name##_##test_name##_body()

// ---------------------------------------------------------------------------
//  Запуск
// ---------------------------------------------------------------------------
struct RunResult {
    std::size_t total = 0;
    std::size_t passed = 0;
    std::vector<std::string> failed;
    double seconds = 0.0;

    [[nodiscard]] int exit_code() const { return failed.empty() ? 0 : 1; }
};

/// Запускает все тесты; filter — подстрока «suite.name» (пустая строка — все).
inline RunResult run_all(std::string_view filter = {}) {
    RunResult result;
    std::string current_suite;
    const auto& entries = Registry::instance().entries();

    for (const auto& entry : entries) {
        const std::string full = entry.suite + "." + entry.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) continue;

        if (entry.suite != current_suite) {
            current_suite = entry.suite;
            std::cout << "\n" << yellow() << "▸ " << current_suite << reset() << "\n";
        }
        ++result.total;

        try {
            entry.body();
            ++result.passed;
            std::cout << "  " << green() << "✓" << reset() << " " << entry.name << "\n";
        } catch (const AssertionFailure& failure) {
            result.failed.push_back(full + ": " + failure.message);
            std::cout << "  " << red() << "✗ " << entry.name << reset() << "\n"
                      << "      " << failure.message << "\n";
        } catch (const std::exception& error) {
            result.failed.push_back(full + ": исключение: " + error.what());
            std::cout << "  " << red() << "✗ " << entry.name << reset()
                      << "  (исключение: " << error.what() << ")\n";
        } catch (...) {
            result.failed.push_back(full + ": неизвестное исключение");
            std::cout << "  " << red() << "✗ " << entry.name << reset()
                      << "  (неизвестное исключение)\n";
        }
    }

    std::cout << "\n";
    if (result.failed.empty()) {
        std::cout << green() << "Все тесты прошли: " << result.passed << "/" << result.total
                  << reset() << "\n";
    } else {
        std::cout << red() << "Провалено " << result.failed.size() << " из " << result.total
                  << reset() << "\n";
        for (const auto& item : result.failed) std::cout << "  • " << item << "\n";
    }
    return result;
}

}  // namespace aia::test

// ---------------------------------------------------------------------------
//  Точка входа тестового бинарника (в одном файле на проект)
// ---------------------------------------------------------------------------
#define AIA_TEST_MAIN \
    int main(int argc, char** argv) {                                       \
        const std::string filter = argc > 1 ? std::string(argv[1]) : "";    \
        const auto result = ::aia::test::run_all(filter);                   \
        return result.exit_code();                                          \
    }
