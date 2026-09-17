#pragma once

// Archivos subidos, con reglas en el mismo lenguaje que `validation`.
//
//   const syrax::Uploads archivos{request};
//
//   const auto fallos = archivos.check({
//       syrax::upload("avatar").required().maxSize(2 * 1024 * 1024).image(),
//       syrax::upload("cv").maxSize(5 * 1024 * 1024).extensions({"pdf"}).pdf(),
//   });
//   if (!fallos.empty()) co_return syrax::Error{422, "validation failed", "", "", fallos};
//
//   const auto avatar = archivos.file("avatar");   // std::optional<Upload>
//
// Devuelve `FieldError`, el mismo tipo que devuelve validate() sobre un
// request: asi el 422 sale por el camino de siempre, con el mismo formato, y
// un proyecto que sobrescribio onError no tiene que enterarse de que existe
// esto.
//
// **Lo que NO entra: guardar.** Drogon trae un saveAs() y no se envuelve a
// proposito. Guardar un archivo es elegir donde -disco local, S3, un volumen
// compartido-, como se nombra para que dos subidas no se pisen, quien lo borra
// y que pasa cuando el disco se llena. Eso es storage, y storage no tiene
// superficie finita. El contenido esta en `bytes`; a partir de ahi el proyecto
// hace lo suyo, incluido `file->drogon().saveAs(...)` si lo que quiere es el
// disco local.
//
// **No hay un filtro por tipo MIME, y es deliberado.** El tipo que viaja en el
// multipart lo declara el CLIENTE: subir un .php diciendo que es image/png es
// el ataque de manual, asi que un `mimes({"image/png"})` da una sensacion de
// seguridad que no corresponde a nada. Lo que hay son dos cosas que si
// significan algo:
//
//   extensions()  filtro de conveniencia, para rechazar pronto y con buen
//                 mensaje. Tampoco es una frontera: el nombre lo pone el cliente.
//   image()/pdf() comprueban los PRIMEROS BYTES del archivo, que es lo unico
//                 que no se puede falsificar sin falsificar el contenido.
//
// Si de verdad hace falta aceptar un tipo raro, el contenido esta en `bytes` y
// el proyecto sabe reconocerlo mejor que el framework.

#include <syrax/middleware.hpp>
#include <syrax/result.hpp>

#include <drogon/MultiPart.h>
#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace syrax {

// Un archivo que llego en la peticion.
struct Upload {
    std::string      field;     // el name del input: "avatar"
    std::string      filename;  // como lo llamo el cliente
    std::string      extension;
    std::size_t      size = 0;
    std::string_view bytes;

    const drogon::HttpFile& drogon() const { return *raw_; }

    const drogon::HttpFile* raw_ = nullptr;
};

namespace detail {

inline std::string lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

// Las firmas que se pueden comprobar sin ambigüedad. Se queda en estas cinco a
// proposito: una tabla de cien tipos es una libreria de deteccion de formatos,
// y a partir de cierto punto los falsos negativos hacen mas daño que bien.
inline bool startsWith(std::string_view bytes, std::string_view magic) {
    return bytes.size() >= magic.size() && bytes.substr(0, magic.size()) == magic;
}

inline bool looksLikePng(std::string_view b) { return startsWith(b, "\x89PNG\r\n\x1a\n"); }
inline bool looksLikeJpeg(std::string_view b) { return startsWith(b, "\xFF\xD8\xFF"); }
inline bool looksLikeGif(std::string_view b) { return startsWith(b, "GIF87a") || startsWith(b, "GIF89a"); }
inline bool looksLikePdf(std::string_view b) { return startsWith(b, "%PDF-"); }

inline bool looksLikeWebp(std::string_view b) {
    return b.size() >= 12 && startsWith(b, "RIFF") && b.substr(8, 4) == "WEBP";
}

inline bool looksLikeImage(std::string_view b) {
    return looksLikePng(b) || looksLikeJpeg(b) || looksLikeGif(b) || looksLikeWebp(b);
}

}  // namespace detail

// Lo que se le exige a un campo de archivo.
class FileRule {
public:
    explicit FileRule(std::string field) : field_{std::move(field)} {}

    // Sin esto, un campo que no llega simplemente no se comprueba: hay
    // formularios donde el avatar es opcional y no llega nada.
    FileRule& required(bool value = true) {
        required_ = value;
        return *this;
    }

    FileRule& maxSize(std::size_t bytes) {
        max_ = bytes;
        return *this;
    }

    FileRule& minSize(std::size_t bytes) {
        min_ = bytes;
        return *this;
    }

    FileRule& extensions(std::vector<std::string> allowed) {
        for (auto& e : allowed) e = detail::lower(std::move(e));
        extensions_ = std::move(allowed);
        return *this;
    }

    // Comprueba los PRIMEROS BYTES, no la cabecera: png, jpeg, gif o webp de
    // verdad. Es lo que no se puede falsificar sin falsificar el contenido.
    FileRule& image(bool value = true) {
        image_ = value;
        return *this;
    }

    FileRule& pdf(bool value = true) {
        pdf_ = value;
        return *this;
    }

    const std::string& field() const { return field_; }

    // Corre las reglas. Devuelve TODOS los fallos del campo, no el primero,
    // igual que validate().
    std::vector<FieldError> check(const std::optional<Upload>& file) const {
        std::vector<FieldError> errors;

        const auto fail = [&](std::string message) {
            errors.push_back({field_, std::move(message)});
        };

        if (!file) {
            if (required_) fail("no se envio ningun archivo");
            return errors;
        }

        if (max_ && file->size > *max_) {
            fail("pesa " + std::to_string(file->size) + " bytes y el maximo son " +
                 std::to_string(*max_));
        }
        if (min_ && file->size < *min_) {
            fail("pesa " + std::to_string(file->size) + " bytes y el minimo son " +
                 std::to_string(*min_));
        }

        // Un archivo de cero bytes pasa cualquier limite de tamaño maximo y
        // casi nunca es lo que alguien queria subir.
        if (file->size == 0) fail("esta vacio");

        if (!extensions_.empty() &&
            std::ranges::find(extensions_, detail::lower(file->extension)) == extensions_.end()) {
            fail("la extension '" + file->extension + "' no esta permitida");
        }

        if (image_ && !detail::looksLikeImage(file->bytes)) {
            fail("no es una imagen: el contenido no empieza por la firma de png, jpeg, gif ni webp");
        }
        if (pdf_ && !detail::looksLikePdf(file->bytes)) {
            fail("no es un pdf: el contenido no empieza por %PDF-");
        }
        return errors;
    }

private:
    std::string                field_;
    bool                       required_ = false;
    bool                       image_    = false;
    bool                       pdf_      = false;
    std::optional<std::size_t> max_;
    std::optional<std::size_t> min_;
    std::vector<std::string>   extensions_;
};

inline FileRule upload(std::string field) { return FileRule{std::move(field)}; }

// Los archivos de una peticion.
//
// Es dueña del parser a proposito. `bytes` es una vista sobre el buffer que
// este objeto mantiene vivo: con un parser compartido o estatico, la vista
// apuntaria a los bytes de OTRA peticion en cuanto llegara la siguiente. Y un
// thread_local seria todavia peor, porque un co_await reanuda la corrutina en
// otro hilo y el fallo no daria un error, daria el archivo del usuario
// equivocado de vez en cuando.
class Uploads {
public:
    explicit Uploads(const Request& request)
        : parser_{std::make_shared<drogon::MultiPartParser>()} {
        if (parser_->parse(request.drogon()) != 0) return;

        for (const auto& file : parser_->getFiles()) {
            files_.push_back(Upload{
                .field     = file.getItemName(),
                .filename  = file.getFileName(),
                .extension = std::string{file.getFileExtension()},
                .size      = file.fileLength(),
                .bytes     = file.fileContent(),
                .raw_      = &file,
            });
        }
    }

    std::optional<Upload> file(const std::string& field) const {
        const auto it = std::ranges::find(files_, field, &Upload::field);
        if (it == files_.end()) return std::nullopt;
        return *it;
    }

    const std::vector<Upload>& all() const { return files_; }
    bool                       empty() const { return files_.empty(); }

    // Aplica las reglas y devuelve TODOS los fallos juntos, en el mismo tipo
    // que devuelve validate(): asi el 422 sale por el camino de siempre.
    std::vector<FieldError> check(const std::vector<FileRule>& rules) const {
        std::vector<FieldError> errors;

        for (const auto& rule : rules) {
            auto fallos = rule.check(file(rule.field()));
            errors.insert(errors.end(), fallos.begin(), fallos.end());
        }
        return errors;
    }

private:
    std::shared_ptr<drogon::MultiPartParser> parser_;
    std::vector<Upload>                      files_;
};

}  // namespace syrax
