#pragma once

#include <string>
#include <utility>
#include <variant>

namespace syrax {

// Un fallo de negocio. No es una excepcion: viaja por valor y el framework
// lo traduce a una respuesta HTTP en el borde.
struct Error {
    int         status;
    std::string message;
};

inline Error BadRequest(std::string m)   { return {400, std::move(m)}; }
inline Error Unauthorized(std::string m) { return {401, std::move(m)}; }
inline Error Forbidden(std::string m)    { return {403, std::move(m)}; }
inline Error NotFound(std::string m)     { return {404, std::move(m)}; }
inline Error Conflict(std::string m)     { return {409, std::move(m)}; }
inline Error Unprocessable(std::string m){ return {422, std::move(m)}; }
inline Error Internal(std::string m)     { return {500, std::move(m)}; }

// Lo que devuelve un handler: el valor, o el error. Ambos constructores son
// implicitos a proposito, para que `return user;` y `return NotFound("...")`
// funcionen sin ceremonia.
template <typename T>
class Result {
public:
    Result(T v) : v_(std::move(v)) {}
    Result(Error e) : v_(std::move(e)) {}

    bool         ok() const noexcept { return v_.index() == 0; }
    const T&     value() const       { return std::get<0>(v_); }
    const Error& error() const       { return std::get<1>(v_); }

private:
    std::variant<T, Error> v_;
};

}  // namespace syrax
