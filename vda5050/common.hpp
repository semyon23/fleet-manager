#pragma once

// VDA 5050 v3.0.0 — общие вещи для всех сообщений:
//   - хелперы сериализации (обязательные / optional поля)
//   - макрос для строковых enum'ов (неизвестное значение -> исключение)
//   - заголовок сообщения (headerId, timestamp, version, manufacturer, serialNumber)
//   - генерация timestamp в формате спецификации
//   - сравнение структур (==, !=) по eq_key()

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace vda5050 {

using json = nlohmann::json;

// Версия протокола, которую пишем в поле `version` исходящих сообщений.
inline constexpr const char* kProtocolVersion = "3.0.0";

namespace detail {

// --- запись ---

template <typename T>
void put(json& j, const char* key, const T& value) {
    j[key] = value;
}

// optional: если значения нет — поле не пишем совсем (а не null).
template <typename T>
void put(json& j, const char* key, const std::optional<T>& value) {
    if (value) {
        j[key] = *value;
    }
}

// --- чтение ---

// Обязательное поле: нет поля или не тот тип -> nlohmann::json::exception.
template <typename T>
void get(const json& j, const char* key, T& value) {
    j.at(key).get_to(value);
}

// Optional поле: отсутствует или null -> std::nullopt.
template <typename T>
void get(const json& j, const char* key, std::optional<T>& value) {
    auto it = j.find(key);
    if (it != j.end() && !it->is_null()) {
        value = it->template get<T>();
    } else {
        value.reset();
    }
}

template <typename E>
std::string enum_to_string(E value) {
    for (const auto& [e, s] : enum_table(value)) {
        if (e == value) {
            return s;
        }
    }
    throw std::invalid_argument("vda5050: enum value is missing in the string table");
}

template <typename E>
E enum_from_string(const std::string& str, const char* type_name) {
    for (const auto& [e, s] : enum_table(E{})) {
        if (s == str) {
            return e;
        }
    }
    throw std::invalid_argument(std::string("vda5050: unknown ") + type_name + " value '" + str + "'");
}

}  // namespace detail

// Строковый enum <-> enum class.
// В отличие от NLOHMANN_JSON_SERIALIZE_ENUM неизвестная строка не превращается
// молча в первое значение, а бросает std::invalid_argument — для протокола это важно.
// Дополнительно генерирует to_string(E) для логов.
#define VDA5050_JSON_ENUM(ENUM_TYPE, ...)                                                   \
    inline const std::vector<std::pair<ENUM_TYPE, std::string>>& enum_table(ENUM_TYPE) {    \
        static const std::vector<std::pair<ENUM_TYPE, std::string>> table = __VA_ARGS__;    \
        return table;                                                                       \
    }                                                                                       \
    inline std::string to_string(ENUM_TYPE value) { return detail::enum_to_string(value); } \
    inline void to_json(json& j, const ENUM_TYPE& value) { j = detail::enum_to_string(value); } \
    inline void from_json(const json& j, ENUM_TYPE& value) {                                \
        value = detail::enum_from_string<ENUM_TYPE>(j.get<std::string>(), #ENUM_TYPE);      \
    }

// Сравнение структур (C++17, без defaulted operator==).
// Структура становится сравниваемой, если рядом с ней объявлена
//     inline auto eq_key(const T& v) { return std::tie(v.a, v.b, ...); }
// Тогда a == b <=> eq_key(a) == eq_key(b). Поля, не попавшие в eq_key, в сравнении не участвуют.
template <typename T, typename = decltype(eq_key(std::declval<const T&>()))>
bool operator==(const T& a, const T& b) {
    return eq_key(a) == eq_key(b);
}

template <typename T, typename = decltype(eq_key(std::declval<const T&>()))>
bool operator!=(const T& a, const T& b) {
    return !(a == b);
}

// Заголовок (7.2). В JSON это не отдельный объект: поля лежат на верхнем уровне сообщения.
struct Header {
    std::uint32_t header_id = 0;  // свой счётчик на каждый топик, +1 на каждое отправленное сообщение
    std::string timestamp;        // ISO 8601 UTC: "2017-04-15T11:40:03.123Z", см. make_timestamp()
    std::string version = kProtocolVersion;
    std::string manufacturer;
    std::string serial_number;
};

inline void put_header(json& j, const Header& h) {
    detail::put(j, "headerId", h.header_id);
    detail::put(j, "timestamp", h.timestamp);
    detail::put(j, "version", h.version);
    detail::put(j, "manufacturer", h.manufacturer);
    detail::put(j, "serialNumber", h.serial_number);
}

inline void get_header(const json& j, Header& h) {
    detail::get(j, "headerId", h.header_id);
    detail::get(j, "timestamp", h.timestamp);
    detail::get(j, "version", h.version);
    detail::get(j, "manufacturer", h.manufacturer);
    detail::get(j, "serialNumber", h.serial_number);
}

// Timestamp в формате спецификации: YYYY-MM-DDTHH:mm:ss.fffZ (UTC).
inline std::string make_timestamp(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms));
    return out;
}

}  // namespace vda5050
