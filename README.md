# Syrax

**Un framework de APIs para C++ moderno.** Construido sobre Drogon (HTTP) y Glaze (tipos y JSON).

> **Estado: diseño.** Nada de esto está implementado todavía. Este documento se escribió *antes*
> del código a propósito — si la API no se puede explicar de forma limpia aquí, tampoco va a
> sentirse limpia al usarla.

---

## Qué es

Syrax hace que crear una API REST en C++ se sienta como hacerlo en FastAPI, sin dejar de ser C++ y
sin quitarte acceso a nada.

Es un framework en el sentido de **FastAPI, no de Laravel**. Ambos invierten el control — tú
registras handlers, el framework corre el loop. La diferencia está en cuánto poseen: Laravel te
dicta estructura de directorios, ORM, ciclo de vida y configuración. FastAPI posee el ruteo y la
validación, y nada más.

Syrax posee cinco cosas. El resto es tuyo.

---

## Instalacion

```bash
git clone https://github.com/KeevDev/Syrax.git
cd Syrax
./install.sh
```

Instala el comando `syrax` en `~/.local/bin` — sin sudo. Tarda **unos segundos**:
el CLI no enlaza contra la libreria, asi que no descarga ni compila Drogon.

```bash
./install.sh --prefix /usr/local   # otro destino
./install.sh --add-to-path         # ademas lo agrega a tu shell rc
./install.sh --uninstall           # lo quita
```

Requiere CMake 3.25+, git y un compilador con C++23 (GCC 14+ o Clang 17+).

### Primer proyecto

```bash
syrax new mi-api
cd mi-api
syrax serve
```

```
$ curl localhost:8080/hello/kevin
{"message":"hello, kevin"}
```

La primera compilacion tarda ~3 minutos porque baja y compila Drogon; las
siguientes son de segundos.

### Comandos

| | |
|---|---|
| `syrax new <nombre>` | crea un proyecto |
| `syrax build` | configura y compila |
| `syrax serve [--port N]` | compila y levanta el servidor |
| `syrax version` | version instalada |

---

## El problema

Un endpoint en Drogon hoy:

```cpp
class UserController : public drogon::HttpController<UserController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UserController::createUser, "/users", drogon::Post);
    METHOD_LIST_END

    void createUser(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback) {
        auto json = req->getJsonObject();
        if (!json) { /* armar error 400 a mano */ }
        if (!json->isMember("name")) { /* armar error a mano */ }
        auto name = (*json)["name"].asString();
        if (name.length() < 3) { /* armar error a mano */ }
        // ...repetir por cada campo

        Json::Value out;
        out["id"] = 1;
        callback(HttpResponse::newHttpJsonResponse(out));
    }
};
```

Veinticinco líneas, validación a mano y propensa a errores, cero documentación generada.

El mismo endpoint en Syrax:

```cpp
app.post("/users", [](CreateUser req) -> Result<User> {
    return User{ .id = save(req), .name = req.name, .email = req.email };
});
```

Esa comparación **es** el producto. No hay que explicar la propuesta de valor: cualquiera que haya
escrito Drogon la entiende en el acto. Y es medible — se cuentan líneas antes y después.

---

## Cómo se ve

```cpp
#include <syrax/syrax.hpp>

struct CreateUser {
    std::string name;
    std::string email;
    int         age;
};

struct User {
    int64_t     id;
    std::string name;
    std::string email;
};

int main() {
    syrax::App app;

    app.post("/users", [](CreateUser req) -> Result<User> {
        if (emailExists(req.email))
            return Conflict("email already registered");

        return User{ .id = save(req), .name = req.name, .email = req.email };
    });

    app.get("/users/{id}", [](int64_t id) -> Result<User> {
        auto user = findUser(id);
        if (!user) return NotFound("user not found");
        return *user;
    });

    app.run(8080);
}
```

`cmake --build build && ./app` → API corriendo, con validación aplicada y OpenAPI en `/docs`.

Sin macros. Sin heredar de nada. Sin registrar rutas en otro archivo. Sin escribir YAML.

### Validación por tipos

Las constraints viven en el tipo, no en un bloque aparte:

```cpp
struct CreateUser {
    MinLen<3, std::string>  name;
    Email                   email;
    Range<0, 150, int>      age;
};
```

Ventajas sobre un DSL de validación separado: la constraint viaja con el dato y no puede
desincronizarse de la definición, se auto-documenta, genera sola las constraints del OpenAPI, y
aprovecha el sistema de tipos de C++ en vez de esconderlo.

Un request inválido produce un 422 con paths y mensajes, sin que escribas nada:

```json
{
  "errors": [
    { "path": "/name",  "message": "must be at least 3 characters" },
    { "path": "/email", "message": "invalid email format" }
  ]
}
```

### El escape hatch

Siempre puedes bajarte a Drogon puro, a media función, sin pelear con nada:

```cpp
app.post("/upload", [](syrax::Raw raw) -> Result<Ack> {
    const drogon::HttpRequestPtr& req = raw.drogon();   // Drogon, sin intermediarios
    // ...
});
```

Esto no es una nota al pie. Es una restricción de diseño dura: **el día que Syrax atrape a alguien,
es peor que no existir.**

---

## Arquitectura

La tesis central es que Syrax escribe muy poco código. Las partes difíciles ya existen, son
excelentes, y las mantiene alguien más.

```
┌──────────────────────────────────────────────────┐
│  Tu aplicación                                   │
├──────────────────────────────────────────────────┤
│  SYRAX  ← lo único que construimos               │
│                                                  │
│  1. Ruteo con deducción de tipos desde la firma  │
│  2. Binding request → struct, con validación     │
│  3. Errores de validación → 422 estructurado     │
│  4. Ensamblado del documento OpenAPI             │
│  5. Onboarding: syrax new → corriendo en 5 min   │
├─────────────────────────┬────────────────────────┤
│  GLAZE                  │  DROGON                │
│  reflection, JSON,      │  HTTP, async,          │
│  JSON Schema            │  corrutinas, sockets   │
└─────────────────────────┴────────────────────────┘
```

**Cinco cosas.** Todo lo demás se delega.

Glaze hace reflection sobre structs agregados sin macros, lo que significa que los DTOs son structs
normales de C++ — no hay `DESCRIBE`, ni `DTO_FIELD`, ni herencia. Ese es el detalle que hace posible
el ejemplo de arriba hoy, sin esperar a C++26.

### El segundo problema: el build

Levantar un proyecto C++ con servidor HTTP, JSON y tests en una máquina limpia es un calvario de
horas. Ningún framework de C++ trata esto como feature. **Syrax sí**, porque es la mitad de lo que
significa "fácil".

Objetivo medible: en una máquina limpia,

```bash
syrax new my-api && cd my-api && syrax serve
```

funciona en **menos de 5 minutos**, sin configurar nada.

---

## Principios de diseño

Tres reglas. Existen para que el proyecto no vuelva a crecer.

### 1. Dramático, no marginal

Si Syrax ahorra 20% del código, nadie va a cambiar nada. El listón es **5x o no vale la pena**.
Se mide contra eso, no contra "quedó más lindo".

### 2. La abstracción nunca es una cárcel

Todo handler puede bajarse a Drogon crudo. Todo componente puede reemplazarse. Syrax agrega
ergonomía sobre Drogon; no lo oculta ni lo envuelve de forma que estorbe.

### 3. Regla de admisión

Una feature entra sólo si hace *notablemente más fácil crear una API*. No si "sería cómodo
tenerla". Cada vez que tiente meter ORM, colas o auth, es el framework grande tratando de resucitar.

---

## No-objetivos

Explícitamente fuera de alcance, indefinidamente:

```
ORM propio          Colas / Jobs        Scheduler
Modelos/relaciones  Event bus           Service discovery
Migraciones         gRPC                Load balancing
Sistema de auth     WebSockets / SSE    Circuit breakers
Mail                Storage / S3        Notifications
```

Si un proyecto que usa Syrax necesita algo de esto, lo toma de una librería existente.
**Syrax compone; no reemplaza.**

Sobre bases de datos: no se construye un ORM. Un ORM es un proyecto multi-año por sí solo y es
donde mueren los frameworks.

---

## Decisiones abiertas

| # | Decisión | Punto de partida |
|---|---|---|
| 1 | Modelo de errores | `std::expected` interno; excepciones sólo en el borde HTTP |
| 2 | Modelo de threading | Documentar el contrato explícito: qué es thread-safe, qué es por request |
| 3 | Contexto de request | Explícito por parámetro. **Nunca `thread_local`** — las corrutinas migran de hilo |
| 4 | Estándar C++ | C++20 base (corrutinas, concepts) |
| 5 | Distribución | Semver + decidir estática / compartida / header-only |
| 6 | Idioma del repo | Si se busca adopción, el README público tendrá que ser inglés |

---

## Roadmap

Cada milestone es útil y publicable solo. Si el proyecto se abandona en cualquier punto, lo hecho
sirve.

### M0 — Spike · *decide todo*

Hacer compilar y correr **exactamente** el ejemplo de arriba. Un endpoint, todo hardcodeado donde
haga falta. Es integrar Glaze con Drogon y mirar cómo queda: días, no semanas.

El objetivo no es arquitectura. Es **mirar el código y juzgar honestamente si se siente bien**.

> **Criterio de kill:** si el resultado no es dramáticamente mejor que Drogon pelado, la premisa no
> existe. Mejor saberlo en la semana 1 que en el mes 12.

### M1 — Ruteo y binding

Deducción de tipos desde la firma del handler, path y query params tipados, deserialización y
validación automáticas, mapeo uniforme de errores, middleware, handlers como corrutinas.

### M2 — Validación por tipos

`MinLen`, `MaxLen`, `Pattern`, `Range`, `Email`, `Uuid`. Errores acumulados con JSON Pointer.

### M3 — OpenAPI y docs

`/openapi.json` y `/docs` generados de las rutas y los tipos. Sale casi gratis de M1 + M2 y es el
mayor gancho de adopción.

### M4 — Onboarding

`syrax new`, `syrax serve`, dependencias precompiladas, proyecto que compila de una.
**Tratarlo como feature, no como tarea de infra** — es donde vive la mitad de la diferenciación, y
es la parte que todo el mundo deja para después y nunca hace.

---

## Cómo se mide el éxito

En orden. No por features:

1. ¿El código del M0 es dramáticamente mejor que Drogon pelado? *(si no → parar)*
2. ¿Un desconocido logró `syrax new` → API corriendo sin pedir ayuda?
3. ¿Alguien externo lo usó sin que se lo pidieras?
4. ¿Alguien reportó un bug que sólo se encuentra usándolo en producción?

---

## Sobre el nombre

Drogon, Syrax — los dos son dragones. Drogon es el motor HTTP sobre el que esto vuela.

---

## Contexto

El alcance original de este proyecto era un framework completo de aplicaciones y sistemas
distribuidos para C++ — 67 secciones, 10 fases, ORM propio, colas, gRPC, event bus, service
discovery. Se descartó por tres razones:

1. **Ya existe.** userver (Yandex, open source) implementa esa visión y corre cientos de servicios
   en producción.
2. **Mal encaje cultural.** C++ adopta librerías, no frameworks que exigen reestructurar la app.
3. **Aritmética de mantenimiento.** ~35 subsistemas propios no son sostenibles fuera de un equipo
   financiado.

Lo que quedó es más chico, más definido y mejor apuntado: el hueco real no es "a C++ le falta un
framework de sistemas distribuidos", es que **nadie ha logrado que hacer una API en C++ se sienta
fácil**. Drogon es potente pero verboso. oat++ lo intentó con la ergonomía de C++ de 2018 — tipos
propios, macros por todas partes, async anterior a las corrutinas. Glaze resolvió los tipos pero no
sabe nada de HTTP.

El pegamento entre ellos no existe. Eso es Syrax.
