#pragma once

#include <glaze/json/schema.hpp>
#include <json/json.h>

#include <string>
#include <vector>

namespace syrax {

// Lo que App recuerda de cada ruta al registrarla. Con esto se arma el
// documento OpenAPI sin que el desarrollador escriba ni anote nada: los
// esquemas salen de los mismos tipos que ya usan los handlers.
struct RouteInfo {
    std::string method;          // "get", "post", ...
    std::string path;            // "/api/v1/users/{id}"
    std::string requestSchema;   // JSON Schema del body, vacio si no hay
    std::string responseSchema;  // JSON Schema de la respuesta
    int         okStatus = 200;
};

namespace detail {

// Convierte el JSON Schema de Glaze en un Json::Value para incrustarlo.
inline Json::Value parseSchema(const std::string& schema) {
    Json::Value  out;
    Json::Reader reader;

    if (schema.empty() || !reader.parse(schema, out)) return Json::objectValue;
    return out;
}

// Saca los {nombres} de un path para declararlos como parametros.
inline std::vector<std::string> pathParams(const std::string& path) {
    std::vector<std::string> names;

    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '{') continue;

        const auto close = path.find('}', i);
        if (close == std::string::npos) break;

        names.push_back(path.substr(i + 1, close - i - 1));
        i = close;
    }
    return names;
}

inline Json::Value jsonContent(const std::string& schema) {
    Json::Value content;
    content["application/json"]["schema"] = parseSchema(schema);
    return content;
}

}  // namespace detail

// Arma el documento OpenAPI 3.1 a partir de las rutas registradas.
inline std::string buildOpenApi(const std::vector<RouteInfo>& routes,
                                const std::string&            title,
                                const std::string&            version) {
    Json::Value doc;
    doc["openapi"]         = "3.1.0";
    doc["info"]["title"]   = title;
    doc["info"]["version"] = version;
    doc["paths"]           = Json::objectValue;

    for (const auto& route : routes) {
        Json::Value operation;

        for (const auto& name : detail::pathParams(route.path)) {
            Json::Value parameter;
            parameter["name"]            = name;
            parameter["in"]              = "path";
            parameter["required"]        = true;
            parameter["schema"]["type"]  = "string";
            operation["parameters"].append(parameter);
        }

        if (!route.requestSchema.empty()) {
            operation["requestBody"]["required"] = true;
            operation["requestBody"]["content"]  = detail::jsonContent(route.requestSchema);
        }

        const auto ok = std::to_string(route.okStatus);
        operation["responses"][ok]["description"] = "ok";
        operation["responses"][ok]["content"] = detail::jsonContent(route.responseSchema);

        // Todo handler puede devolver un Error, asi que el contrato de fallo
        // es uniforme y se documenta solo.
        Json::Value error;
        error["type"]                            = "object";
        error["properties"]["error"]["type"]     = "object";
        error["properties"]["error"]["properties"]["status"]["type"]  = "integer";
        error["properties"]["error"]["properties"]["message"]["type"] = "string";

        Json::Value errorContent;
        errorContent["application/json"]["schema"] = error;

        for (const char* code : {"400", "404", "409", "422", "500"}) {
            operation["responses"][code]["description"] = "error";
            operation["responses"][code]["content"]     = errorContent;
        }

        doc["paths"][route.path][route.method] = operation;
    }

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    return Json::writeString(writer, doc);
}

// Swagger UI servido desde CDN. Ver las docs necesita internet; el
// /openapi.json en cambio es local y sirve sin red.
inline std::string swaggerHtml(const std::string& title) {
    return R"(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>)" + title + R"( — docs</title>
  <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist/swagger-ui.css">
  <style>body{margin:0}</style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://unpkg.com/swagger-ui-dist/swagger-ui-bundle.js"></script>
  <script>
    SwaggerUIBundle({ url: '/openapi.json', dom_id: '#swagger-ui' });
  </script>
</body>
</html>)";
}

}  // namespace syrax
