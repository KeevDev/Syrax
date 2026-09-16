#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace syrax {

// Un campo que no cumplio una regla. Se acumulan todos antes de responder:
// una API que devuelve el primer error obliga a descubrir los demas de a uno.
//
// Vive aqui y no en validation.hpp porque un Error lo lleva dentro: asi el 422
// de validacion es un Error como cualquier otro y sale por el mismo sitio.
struct FieldError {
    std::string field;
    std::string message;
};

// Un fallo de negocio. No es una excepcion: viaja por valor y el framework
// lo traduce a una respuesta HTTP en el borde.
//
// `status` y `message` son lo minimo. Los otros tres son opcionales y solo
// aparecen en el JSON cuando se llenan, para que un cliente que hoy lee
// error.message siga leyendo lo mismo manana:
//
//   code    estable y en ingles de maquina: "saldo_insuficiente". Es sobre
//           esto sobre lo que el cliente ramifica, no sobre el mensaje, que
//           se puede reescribir sin avisar.
//   detail  la version larga para un humano.
//   fields  el detalle por campo de un 422.
struct Error {
    int                     status;
    std::string             message;
    std::string             code;
    std::string             detail;
    std::vector<FieldError> fields;

    // Para anadir el codigo sin repetir el resto:
    //   syrax::Conflict("el email ya existe").as("email_duplicado")
    Error as(std::string value) const& {
        Error copy = *this;
        copy.code  = std::move(value);
        return copy;
    }

    Error explain(std::string value) const& {
        Error copy  = *this;
        copy.detail = std::move(value);
        return copy;
    }
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
