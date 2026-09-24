// ============================================================================
//  Статусы и результаты: обработка ошибок без исключений.
//
//  Почему так: агент — это долгоживущая система, работающая с файлами, процессами
//  и моделями. Исключения здесь плохи тем, что (а) их легко потерять в глубине
//  вызовов, (б) они мешают писать код, который продолжает работу при частичных
//  сбоях («файл не прочитался — попробуем другой путь»). Поэтому каждый сбой —
//  это значение: Status (код + сообщение + контекст). Исключения остаются только
//  на границе процесса, где мы превращаем их в Status.
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace aia {

// ---------------------------------------------------------------------------
//  Коды ошибок
// ---------------------------------------------------------------------------
enum class ErrCode : std::uint8_t {
    Ok = 0,
    Unknown,
    InvalidArgument,   // неверные параметры вызова
    NotFound,          // нет файла/записи/модели
    AlreadyExists,     // файл уже есть
    Permission,        // доступ запрещён (файл, политика, песочница)
    Io,                // ошибка ввода-вывода
    Parse,             // не разобрался входной формат
    Overflow,          // переполнение/выход за границы
    Timeout,           // не успели по времени
    Cancelled,         // отменено пользователем или родителем
    Unsupported,       // возможность не поддерживается сборкой/платформой
    Network,           // сетевой сбой (используется только веб-UI и MCP)
    Model,             // ошибка инференса: веса, формат, контекст
    Busy,              // ресурс занят
    Interrupted,       // прервано сигналом
    Internal,          // нарушение инварианта — это баг
};

/// Короткое машинное имя кода (для логов, JSON и тестов).
[[nodiscard]] constexpr std::string_view err_code_name(ErrCode code) noexcept {
    switch (code) {
        case ErrCode::Ok:              return "ok";
        case ErrCode::Unknown:         return "unknown";
        case ErrCode::InvalidArgument: return "invalid_argument";
        case ErrCode::NotFound:        return "not_found";
        case ErrCode::AlreadyExists:   return "already_exists";
        case ErrCode::Permission:      return "permission";
        case ErrCode::Io:              return "io";
        case ErrCode::Parse:           return "parse";
        case ErrCode::Overflow:        return "overflow";
        case ErrCode::Timeout:         return "timeout";
        case ErrCode::Cancelled:       return "cancelled";
        case ErrCode::Unsupported:     return "unsupported";
        case ErrCode::Network:         return "network";
        case ErrCode::Model:           return "model";
        case ErrCode::Busy:            return "busy";
        case ErrCode::Interrupted:     return "interrupted";
        case ErrCode::Internal:        return "internal";
    }
    return "unknown";
}

/// Русское описание кода — попадает в сообщения пользователю.
[[nodiscard]] constexpr std::string_view err_code_title(ErrCode code) noexcept {
    switch (code) {
        case ErrCode::Ok:              return "успешно";
        case ErrCode::Unknown:         return "неизвестная ошибка";
        case ErrCode::InvalidArgument: return "неверный аргумент";
        case ErrCode::NotFound:        return "не найдено";
        case ErrCode::AlreadyExists:   return "уже существует";
        case ErrCode::Permission:      return "нет доступа";
        case ErrCode::Io:              return "ошибка ввода-вывода";
        case ErrCode::Parse:           return "ошибка разбора";
        case ErrCode::Overflow:        return "переполнение";
        case ErrCode::Timeout:         return "истекло время";
        case ErrCode::Cancelled:       return "отменено";
        case ErrCode::Unsupported:     return "не поддерживается";
        case ErrCode::Network:         return "сетевая ошибка";
        case ErrCode::Model:           return "ошибка модели";
        case ErrCode::Busy:            return "занято";
        case ErrCode::Interrupted:     return "прервано";
        case ErrCode::Internal:        return "внутренняя ошибка";
    }
    return "неизвестная ошибка";
}

/// Ретраибельные коды: имеющим смысл повторить операцию после паузы.
[[nodiscard]] constexpr bool err_code_retryable(ErrCode code) noexcept {
    switch (code) {
        case ErrCode::Timeout:
        case ErrCode::Busy:
        case ErrCode::Interrupted:
        case ErrCode::Network:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
//  Status — «либо всё хорошо, либо конкретная ошибка»
// ---------------------------------------------------------------------------
class Status {
public:
    constexpr Status() noexcept = default;

    constexpr Status(ErrCode code, std::string message, std::string context = {})
        : code_(code), message_(std::move(message)), context_(std::move(context)) {}

    /// Фабрика «всё хорошо». Отдельное имя нужно, чтобы не конфликтовать с `ok()`.
    [[nodiscard]] static constexpr Status success() noexcept { return Status{}; }

    [[nodiscard]] static Status error(ErrCode code, std::string message, std::string context = {}) {
        return Status{code, std::move(message), std::move(context)};
    }

    [[nodiscard]] static Status invalid(std::string message, std::string context = {}) {
        return Status{ErrCode::InvalidArgument, std::move(message), std::move(context)};
    }
    [[nodiscard]] static Status not_found(std::string message, std::string context = {}) {
        return Status{ErrCode::NotFound, std::move(message), std::move(context)};
    }
    [[nodiscard]] static Status io(std::string message, std::string context = {}) {
        return Status{ErrCode::Io, std::move(message), std::move(context)};
    }
    [[nodiscard]] static Status parse(std::string message, std::string context = {}) {
        return Status{ErrCode::Parse, std::move(message), std::move(context)};
    }
    [[nodiscard]] static Status internal(std::string message, std::string context = {}) {
        return Status{ErrCode::Internal, std::move(message), std::move(context)};
    }
    [[nodiscard]] static Status cancelled(std::string message = "отменено", std::string context = {}) {
        return Status{ErrCode::Cancelled, std::move(message), std::move(context)};
    }

    [[nodiscard]] constexpr bool ok() const noexcept { return code_ == ErrCode::Ok; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] constexpr ErrCode code() const noexcept { return code_; }
    [[nodiscard]] constexpr std::string_view name() const noexcept { return err_code_name(code_); }
    [[nodiscard]] constexpr std::string_view title() const noexcept { return err_code_title(code_); }
    [[nodiscard]] constexpr bool retryable() const noexcept { return err_code_retryable(code_); }

    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const std::string& context() const noexcept { return context_; }

    /// Меняет сообщение, сохраняя код.
    Status& set_message(std::string message) { message_ = std::move(message); return *this; }

    /// Добавляет контекст («fs::write → /etc/hosts»). Несколько уровней склеиваются.
    Status& add_context(std::string context) {
        if (context.empty()) return *this;
        if (context_.empty()) context_ = std::move(context);
        else context_ = std::move(context) + " → " + context_;
        return *this;
    }

    [[nodiscard]] Status with_context(std::string context) const {
        Status copy = *this;
        copy.add_context(std::move(context));
        return copy;
    }

    /// Оставить как есть, если ошибка уже есть; иначе — новая. Нужно в цепочках проверок.
    [[nodiscard]] const Status& or_else(const Status& other) const noexcept {
        return ok() ? other : *this;
    }

    /// Одна строка для логов: «parse: неожиданный символ (json: строка 4)».
    [[nodiscard]] std::string to_string() const {
        if (ok()) return "ok";
        std::string out;
        out.reserve(name().size() + message_.size() + context_.size() + 8);
        out += name();
        out += ": ";
        out += message_.empty() ? std::string(title()) : message_;
        if (!context_.empty()) {
            out += " (";
            out += context_;
            out += ')';
        }
        return out;
    }

private:
    ErrCode code_ = ErrCode::Ok;
    std::string message_{};
    std::string context_{};
};

[[nodiscard]] inline bool operator==(const Status& a, const Status& b) noexcept {
    return a.ok() == b.ok() && (a.ok() || (a.code() == b.code() && a.message() == b.message()));
}
[[nodiscard]] inline bool operator!=(const Status& a, const Status& b) noexcept { return !(a == b); }

// ---------------------------------------------------------------------------
//  Result<T> — значение или ошибка
// ---------------------------------------------------------------------------
template <typename T>
class [[nodiscard]] Result {
public:
    Result() : data_(Status::internal("Result без значения")) {}

    Result(T value) : data_(std::move(value)) {}                      // NOLINT(google-explicit-constructor)
    Result(Status status) : data_(std::move(status)) {}               // NOLINT(google-explicit-constructor)

    [[nodiscard]] static Result make(T value) { return Result{std::move(value)}; }
    [[nodiscard]] static Result err(Status status) { return Result{std::move(status)}; }
    [[nodiscard]] static Result err(ErrCode code, std::string message, std::string context = {}) {
        return Result{Status{code, std::move(message), std::move(context)}};
    }

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Status& status() const noexcept {
        static const Status ok_status{};
        return ok() ? ok_status : std::get<Status>(data_);
    }

    [[nodiscard]] T& value() & { return std::get<T>(data_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(data_); }
    [[nodiscard]] T&& value() && { return std::move(std::get<T>(data_)); }

    [[nodiscard]] T value_or(T fallback) const {
        return ok() ? std::get<T>(data_) : std::move(fallback);
    }

    T* operator->() { return &std::get<T>(data_); }
    const T* operator->() const { return &std::get<T>(data_); }
    T& operator*() { return std::get<T>(data_); }
    const T& operator*() const { return std::get<T>(data_); }

    /// Преобразование значения без изменения ошибки.
    template <typename F>
    [[nodiscard]] auto map(F&& fn) const -> Result<std::invoke_result_t<F, const T&>> {
        using U = std::invoke_result_t<F, const T&>;
        if (!ok()) return Result<U>::err(status());
        return Result<U>::make(fn(std::get<T>(data_)));
    }

    /// Разбор «успеха» и «ошибки» одним вызовом.
    template <typename OnOk, typename OnErr>
    [[nodiscard]] auto match(OnOk&& on_ok, OnErr&& on_err) const
        -> std::invoke_result_t<OnOk, const T&> {
        if (ok()) return on_ok(std::get<T>(data_));
        return on_err(status());
    }

private:
    std::variant<T, Status> data_;
};

// ---------------------------------------------------------------------------
//  Утилиты для цепочек проверок
// ---------------------------------------------------------------------------
/// Если status — ошибка, выйти из текущей функции с ним (функция возвращает Status).
#define AIA_RETURN_IF_ERR(status_expr)                       \
    do {                                                     \
        const ::aia::Status aia_status_tmp_ = (status_expr); \
        if (!aia_status_tmp_.ok()) return aia_status_tmp_;   \
    } while (false)

/// Развернуть Result<T>: при ошибке — выйти из функции с этим же Status.
/// Используется как: AIA_TRY(auto text, read_file(path));
#define AIA_TRY_IMPL(value_name, result_expr, unique_suffix)                            \
    auto aia_result_##unique_suffix = (result_expr);                                    \
    if (!aia_result_##unique_suffix.ok()) return aia_result_##unique_suffix.status();    \
    value_name = std::move(*aia_result_##unique_suffix)

#define AIA_TRY(value_name, result_expr) AIA_TRY_IMPL(value_name, result_expr, __COUNTER__)

/// Защита от необработанного Status: в релизе — no-op, в отладке — падаем громко.
inline void status_ignore(const Status&) noexcept {}

// ---------------------------------------------------------------------------
//  Сведение чужого результата к Status (для API, возвращающих bool/errno)
// ---------------------------------------------------------------------------
[[nodiscard]] inline Status from_errno(int err, std::string_view what) {
    if (err == 0) return Status::success();
    std::string message = std::string(what);
    message += ": код ";
    message += std::to_string(err);
    return Status{ErrCode::Io, std::move(message)};
}

[[nodiscard]] inline Status from_bool(bool success, ErrCode code, std::string message) {
    return success ? Status::success() : Status{code, std::move(message)};
}

}  // namespace aia
