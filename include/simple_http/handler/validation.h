#pragma once

// Request-body / parameter validation.
//
// The library does not impose a validation DSL. A type opts in by specializing
// Validator<T>; the Valid<E> extractor (see extract.h) runs it after the value
// has been parsed, and turns a failure into a 422 with a structured field-error
// list. The default Validator<T> accepts everything, so validation is opt-in
// per type and costs nothing for types that do not use it.
//
//     template <> struct simple_http::Validator<CreateUser> {
//         static std::expected<void, std::vector<FieldError>>
//         validate(const CreateUser &v) {
//             std::vector<FieldError> errors;
//             push_if(errors, required_field("name", !v.name.empty()));
//             push_if(errors, range("age", v.age, 1, 150));
//             return errors.empty() ? std::expected<void, std::vector<FieldError>>{}
//                                   : std::unexpected(std::move(errors));
//         }
//     };
//
// The helpers below are convenience predicates, not a schema language. A
// project that already uses a validator library can adapt it in one function.

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace simple_http {

// One field-level validation failure. `field` is the JSON/path parameter name,
// `code` is a stable machine-readable token, and `message` is for humans.
struct FieldError {
    std::string field;
    std::string code;
    std::string message;
};

// The result of a validation: an empty error list means success. A tiny value
// type rather than expected<void, ...> keeps the C++17 port free of another
// dependency while preserving the "check, then read errors" shape.
struct ValidationResult {
    std::vector<FieldError> errors;

    [[nodiscard]] bool has_value() const { return errors.empty(); }
    [[nodiscard]] explicit operator bool() const { return errors.empty(); }
    std::vector<FieldError> &error() { return errors; }
    const std::vector<FieldError> &error() const { return errors; }
};

// The opt-in validation hook. The primary template accepts everything; a
// caller specializes it for a type it wants validated.
template <typename T> struct Validator {
    static ValidationResult validate(const T &) { return {}; }
};

// --- convenience predicates -------------------------------------------------
//
// Each returns an optional<FieldError>: push it only when it has a value.

inline std::optional<FieldError> required_field(std::string field, bool present, std::string message = "required") {
    if (present) {
        return std::nullopt;
    }
    return FieldError{std::move(field), "required", std::move(message)};
}

inline std::optional<FieldError> min_length(std::string field, std::string_view value, std::size_t min) {
    if (value.size() >= min) {
        return std::nullopt;
    }
    return FieldError{std::move(field), "min_length", "must contain at least " + std::to_string(min) + " characters"};
}

inline std::optional<FieldError> max_length(std::string field, std::string_view value, std::size_t max) {
    if (value.size() <= max) {
        return std::nullopt;
    }
    return FieldError{std::move(field), "max_length", "must contain at most " + std::to_string(max) + " characters"};
}

template <typename T> std::optional<FieldError> range(std::string field, const T &value, const T &low, const T &high) {
    if (value >= low && value <= high) {
        return std::nullopt;
    }
    return FieldError{std::move(field), "range", "must be between the allowed bounds"};
}

inline std::optional<FieldError> one_of(std::string field, std::string_view value,
                                        std::initializer_list<std::string_view> allowed) {
    for (std::string_view candidate : allowed) {
        if (value == candidate) {
            return std::nullopt;
        }
    }
    return FieldError{std::move(field), "one_of", "must be one of the allowed values"};
}

// Appends `error` to `errors` when it is present. Lets a validator read as a
// list of checks without repeating the optional dance.
inline void push_if(std::vector<FieldError> &errors, std::optional<FieldError> error) {
    if (error) {
        errors.push_back(std::move(*error));
    }
}

// Renders field errors as the `details` string of the JSON error body. Kept out
// of the error type so callers can replace the whole body shape.
inline std::string field_errors_json(const std::vector<FieldError> &errors) {
    auto escape = [](std::string_view text) {
        std::string out;
        for (char c : text) {
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(static_cast<unsigned char>(c) >> 4) & 0x0F]);
                    out.push_back(hex[static_cast<unsigned char>(c) & 0x0F]);
                } else {
                    out.push_back(c);
                }
            }
        }
        return out;
    };
    std::string out = "[";
    bool first = true;
    for (const auto &error : errors) {
        if (!first)
            out.push_back(',');
        first = false;
        out += "{\"field\":\"" + escape(error.field) + "\",\"code\":\"" + escape(error.code) + "\",\"message\":\"" +
               escape(error.message) + "\"}";
    }
    out.push_back(']');
    return out;
}

} // namespace simple_http
