# Syrax

**Un framework de APIs para C++ moderno.** Construido sobre [Drogon](https://github.com/drogonframework/drogon) (HTTP) y [Glaze](https://github.com/stephenberry/glaze) (tipos y JSON).

```cpp
app.post("/users", [](requests::CreateUser body) -> Task<Result<UserResource>> {
    const auto user = co_await service::create(std::move(body));
    if (!user) co_return Conflict("email already registered");

    co_return resources::from(*user);
});
```

Eso es un endpoint completo: parseo del body, validación, manejo de errores, serialización de la respuesta y documentación OpenAPI. Sin macros, sin heredar de nada, sin anotaciones.

> **Estado: funcional, pre-1.0.** El CRUD, la validación, las migraciones, la base de datos, los WebSockets y OpenAPI funcionan y están cubiertos por tests. La API puede cambiar sin aviso hasta la 1.0.

---

## Instalación

```bash
git clone https://github.com/KeevDev/Syrax.git
cd Syrax
./install.sh
```

Instala el comando `syrax` en `~/.local/bin`, sin sudo, en **unos segundos**: el CLI no enlaza contra la librería, así que no descarga ni compila Drogon.

```bash
./install.sh --prefix /usr/local   # otro destino
./install.sh --add-to-path         # además lo agrega a tu shell rc
./install.sh --uninstall           # lo quita
syrax upgrade                      # actualizar despues
```

Requiere CMake 3.25+, git y un compilador con C++23. La suite corre con **GCC 14** en CI; con **Clang 22** compilan la librería, los tests y el ejemplo. Versiones anteriores puede que sirvan, pero no lo he comprobado.

---

## Empezar

```bash
syrax new mi-api          # pregunta el motor de base de datos
cd mi-api

docker compose up -d      # solo si elegiste postgres
syrax migrate
syrax db:seed             # opcional, datos de ejemplo
syrax serve
```

```
$ curl localhost:8080/health
{"status":"ok"}

$ curl localhost:8080/api/v1/users
[{"id":1,"name":"Ada Lovelace","email":"ada@example.com"}]
```

Y la documentación interactiva en **http://localhost:8080/docs**.

La primera compilación tarda unos minutos porque baja y compila Drogon; las siguientes son de segundos.

---

## Qué trae

### Handlers tipados

La firma del handler es el contrato. Syrax deduce de ella qué parsear, qué validar y qué documentar.

```cpp
// Sin argumentos
app.get("/users", []() -> Task<Result<std::vector<UserResource>>> { ... });

// Path param tipado
app.get("/users/{id}", [](std::int64_t id) -> Task<Result<UserResource>> { ... });

// Body JSON
app.post("/users", [](requests::CreateUser body) -> Task<Result<UserResource>> { ... });

// Path param y body a la vez: el body es siempre el ultimo argumento
app.put("/users/{id}", [](std::int64_t id, requests::UpdateUser body) -> ... { ... });
```

Los handlers pueden ser síncronos (`Result<T>`) o corrutinas (`Task<Result<T>>`).

### Errores como valores

```cpp
if (!user) co_return NotFound("user not found");
```

`BadRequest`, `Unauthorized`, `Forbidden`, `NotFound`, `Conflict`, `Unprocessable`, `Internal`. Todos se traducen a una respuesta JSON uniforme:

```json
{ "error": { "status": 404, "message": "user not found" } }
```

Incluso las rutas inexistentes responden JSON, no la página HTML de Drogon.

### Validación

El body se parsea en modo estricto antes de que el handler se ejecute. Si algo no cuadra, el cliente recibe un **422** y el handler nunca corre:

| | |
|---|---|
| Campo faltante | `missing_key` |
| Campo desconocido | `unknown_key` |
| Tipo incorrecto | `parse_number_failure` |
| JSON malformado | posición exacta del error |

Eso valida la **forma**. Para el **contenido**, el struct declara sus reglas:

```cpp
struct CreateUser {
    std::string name;
    std::string email;
    int         age;

    static auto rules() {
        return syrax::rules(
            field(&CreateUser::name).notEmpty().minLen(2).maxLen(80),
            field(&CreateUser::email).email(),
            field(&CreateUser::age).range(0, 130));
    }
};
```

`rules()` es opcional: sin él, la validación sigue siendo solo estructural.

Se reportan **todos** los fallos, no el primero:

```json
{"error":{"status":422,"message":"validation failed","fields":[
  {"field":"name",  "message":"debe tener al menos 2 caracteres"},
  {"field":"email", "message":"no es un email valido"},
  {"field":"age",   "message":"debe estar entre 0 y 130"}]}}
```

| | |
|---|---|
| Texto | `notEmpty()` `minLen(n)` `maxLen(n)` `email()` `oneOf({...})` `pattern(re, msg)` |
| Números | `min(n)` `max(n)` `range(lo, hi)` |
| Lo demás | `satisfies(predicado, msg)` |

Tres detalles que no son accidentales:

- **El campo se nombra con `&T::campo`, no con un string.** Renombrarlo rompe la compilación en vez de dejar una regla apuntando a algo que ya no existe.
- **Las reglas se encadenan como métodos** en vez de ser funciones sueltas. Dentro de `rules()` los nombres de los campos tapan a los de namespace, así que un campo llamado `email` volvía inutilizable a una función `email()`. Después de un punto no hay colisión posible.
- **Los límites entran solos al `/docs`.** `minLen(2)` se vuelve `minLength: 2` en el JSON Schema, `email()` se vuelve `format: email`. No hay nada que mantener en paralelo.

Un `std::optional<T>` ausente no se valida: "no vino" es asunto de presencia, no de contenido.

### Base de datos sin ORM ni boilerplate

Un modelo es un struct plano:

```cpp
struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;
};
```

```cpp
auto users = co_await db::query<User>("SELECT id, name, email, age FROM users");
auto one   = co_await db::findOne<User>("SELECT ... WHERE id = $1", id);
auto rows  = co_await db::execute("DELETE FROM users WHERE id = $1", id);
auto nuevo = co_await db::returning<User>("INSERT ... RETURNING id, name, email, age", ...);
```

El mapeo columna → campo lo resuelve Glaze en tiempo de compilación por nombre. **No hay que escribirlo ni generar modelos de 500 líneas.** Una columna `NULL` deja el campo en su valor por defecto; usa `std::optional` si necesitas distinguir.

Conservas de Drogon el pool de conexiones, las corrutinas, las transacciones y los prepared statements. Lo que no hay es query builder ni relaciones: el SQL está a la vista.

> **Un tipo que se refleja no puede vivir en un namespace anónimo.** Glaze saca los nombres de los campos a través de una variable `extern`, y un tipo sin enlace no puede nombrarse desde otra unidad de traducción. GCC lo deja pasar; **Clang lo rechaza** con `used but not defined in this translation unit`. Aplica a todo lo que Syrax serializa o mapea: bodies, resources y modelos. Ponlos en un namespace con nombre — que es donde el andamiaje los pone.

### Migraciones en C++

```cpp
struct CreateUsersTable : syrax::Migration {
    std::string name() const override { return "001_create_users"; }

    void up(syrax::Schema& schema) override {
        schema.create("users", [](syrax::Blueprint& table) {
            table.id();
            table.string("name");
            table.string("email").unique();
            table.integer("age").defaultTo("0");
            table.timestamps();
        });
    }

    void down(syrax::Schema& schema) override { schema.drop("users"); }
};
```

El mismo código genera el DDL correcto para PostgreSQL y SQLite. Modificar tablas existentes:

```cpp
schema.table("users", [](syrax::Blueprint& t) {
    t.string("phone").nullable();          // ADD COLUMN
    t.renameColumn("name", "full_name");   // RENAME COLUMN
    t.dropColumn("age");                   // DROP COLUMN
    t.dropIndex("email");                  // DROP INDEX
});
schema.rename("users", "people");
```

```bash
syrax migrate            # aplica las pendientes
syrax migrate:status     # cuales estan aplicadas
syrax migrate:rollback   # revierte la ultima
```

El estado se registra en la tabla `syrax_migrations`. `Schema::raw()` es el escape hatch para SQL que el builder no cubre.

Para **modificar** una columna que ya existe, `.change()` al final del encadenado:

```cpp
schema.table("users", [](Blueprint& t) {
    t.string("email", 320).nullable().change();   // cambia tipo y nulabilidad
    t.integer("age").defaultTo("0").change();
    t.integer("code").nullable().castUsing("code::integer").change();

    t.check("age_no_negativa", "age >= 0");
    t.dropUnique("email");
    t.dropConstraint("vieja");
});
```

> **`.change()` solo funciona en Postgres.** SQLite únicamente soporta `RENAME`, `ADD COLUMN` y `DROP COLUMN`; cambiar un tipo exige reconstruir la tabla entera. Syrax lanza un error que lo dice y apunta a `Schema::raw()` en vez de generar SQL que el motor va a rechazar. Es una limitación de SQLite, no de Syrax.

### OpenAPI automático

`/openapi.json` y `/docs` con Swagger UI, generados de las rutas registradas. **Los esquemas salen de los mismos tipos que usan los handlers**, así que la documentación no puede desincronizarse del código: no hay anotaciones que mantener.

```cpp
app.docs("Mi API", "2.0.0");   // titulo y version
app.withoutDocs();             // apagarlo en produccion
```

### Middleware, autenticación y políticas

Un middleware es una función que recibe la petición y devuelve un `Error` para cortar, o nada para dejar pasar. Sin clases, sin registro global:

```cpp
app.useOnResponse(securityHeaders());                    // a toda respuesta
app.use(rateLimit(100, std::chrono::minutes{1}));        // a toda petición
app.use("/api/v1/admin", auth::bearer(secreto));         // solo bajo ese prefijo
app.cors({.origins = {"https://mi-front.com"}});
```

También hay `requireApiKey(clave)`, que compara en tiempo constante.

**JWT y contraseñas** (HS256 y PBKDF2-SHA256 sobre OpenSSL):

```cpp
const auto token  = auth::sign({.sub = "42", .role = "admin"}, secreto);
const auto claims = auth::verify(token, secreto);        // optional<Claims>

const auto hash = auth::hashPassword("secreto");         // 600 000 iteraciones
const bool ok   = auth::verifyPassword("secreto", hash);
```

`auth::bearer()` verifica el token y deja el sujeto y el rol en la petición. De ahí sale el actor:

```cpp
app.post("/posts/{id}", [](Request req, std::int64_t id, UpdatePost body)
                         -> Task<Result<PostResource>> {
    const auto actor = actorFrom(req);

    if (auto denied = requireRole(actor, "admin", "editor")) co_return *denied;
    // ...
});
```

Una **política** es solo una función que devuelve `optional<Error>`. No hay registro ni resolución por nombre:

```cpp
namespace policies {
std::optional<Error> update(const Actor& actor, const Post& post) {
    if (actor.is("admin"))         return std::nullopt;
    if (post.authorId == actor.id) return std::nullopt;
    return Forbidden("no puedes editar este post");
}
}

if (auto denied = policies::update(actor, post)) co_return *denied;
```

`requireRole` distingue **401** (no autenticado) de **403** (rol insuficiente); confundirlos le dice a un atacante si una credencial es válida. `allowIf` y `denyIf` cubren las condiciones sueltas.

Un sistema completo de roles y permisos es una aplicación, no un framework: esto es lo mínimo para escribirlo sin pelearse.

### WebSockets

```cpp
syrax::Room sala;

app.ws("/chat", {
    .onOpen    = [&](const Socket& s) { sala.join(s); },
    .onMessage = [&](const Socket&, std::string_view text) { sala.broadcast(text); },
    .onClose   = [&](const Socket& s) { sala.leave(s); },
});
```

`Room` es un grupo de conexiones al que se emite de una vez, con su sincronización resuelta: Drogon reparte las conexiones entre varios event loops, así que sin esto cada proyecto tendría que rehacer el registro y su mutex.

`Socket` trae `send`, `sendJson` (el mismo struct que sirve un endpoint REST sirve un mensaje de socket), `close`, `ip` y `set`/`get` para estado por conexión — lo típico es guardar ahí el usuario que quedó autenticado en `onOpen`.

Los middlewares de Syrax **no** corren sobre sockets: operan sobre respuestas HTTP, y un socket deja de tenerlas después del handshake. La autenticación va dentro de `onOpen`.

---

## Estructura de un proyecto

```
CMakeLists.txt            trae syrax por FetchContent, fijado a un tag
config/app.json           ajustes del servidor (versionado)
.env                      credenciales (NO versionado)
.env.example              las mismas claves, sin valores (SI versionado)
docker/Dockerfile         imagen multi-etapa
docker-compose.yml        postgres, si elegiste ese motor
public/                   estaticos
logs/

database/
├── migrations.hpp        declara el registro
├── migrations.cpp        que migraciones existen y en que orden
├── migrations/           las migraciones
├── seeders/              datos de ejemplo
└── factories/            objetos de mentira para tests

src/
├── main.cpp
├── bootstrap/            preparacion de la app
├── routes/               el mapa de la API, versionable
│   ├── routes.cpp        /health y el alta de cada version
│   └── v1.cpp            rutas de /api/v1
├── http/                 TODO lo atado al transporte
│   ├── controllers/User/
│   ├── requests/User/
│   └── resources/User/
├── services/User/        logica de negocio
├── repositories/User/    SQL
└── models/User/          la forma de la tabla
```

**Por qué existe `http/`:** marca un límite real. Si mañana expones la misma lógica por gRPC, esa carpeta se tira entera y `services/`, `repositories/` y `models/` siguen sirviendo sin tocarse.

**Por qué `models/` y `resources/` están separados:** `User` tiene `passwordHash` y `UserResource` no. Un campo privado no puede filtrarse por accidente porque el tipo que se serializa simplemente no lo tiene.

Ninguno de esos nombres los conoce el framework: son archivos C++ normales. Renómbralos o bórralos.

---

## Comandos

| | | |
|---|---|---|
| `syrax new <nombre>` | `n` | crea un proyecto (`--db postgres\|sqlite`) |
| `syrax build` | `b` | configura y compila |
| `syrax serve` | `s` | compila y levanta (`--port N`) |
| `syrax migrate` | `m` | aplica las migraciones pendientes |
| `syrax migrate:rollback` | `m:r` | revierte la última |
| `syrax migrate:status` | `m:s` | muestra cuáles están aplicadas |
| `syrax db:seed` | `seed` | carga `database/seeders/*.sql` |
| `syrax test` | `t` | compila y corre `ctest` (ver nota abajo) |
| `syrax upgrade` | `-u` | recompila e instala la última versión |
| `syrax version` | `-v` | versión y origen |
| `syrax help` | `-h` | esta lista |

> **`syrax test` sirve dentro del repo de Syrax.** Un proyecto recién generado no trae andamiaje de tests —ni `enable_testing()` ni un target—, así que ahí el comando compila y luego `ctest` no encuentra nada. `database/factories/` está puesto para cuando lo traiga.

---

## Tests

```bash
syrax test                     # en el repo de Syrax
ctest --test-dir build         # equivalente
```

102 casos cubriendo el generador de DDL en ambos dialectos, `ALTER TABLE` ejecutado contra SQLite real, el mapeo de filas a structs, las reglas de validación y su anotación del JSON Schema, la generación de OpenAPI, JWT y hashing de contraseñas, middlewares y políticas, la integración HTTP completa (ruteo, binding de body, 422 con detalle por campo, path params, corrutinas, el 404 y el 500 en JSON) y los WebSockets hablando el protocolo a mano contra el servidor real.

CI en GitHub Actions, en cada push y PR:

| job | qué hace |
|---|---|
| `test` | compila con GCC 14 y corre la suite |
| `clang` | lo mismo con Clang — GCC deja pasar cosas que Clang no |
| `docker` | genera un proyecto, construye su `Dockerfile` y comprueba que la imagen arranca y responde en `/health` |

> Los jobs `clang` y `docker` se añadieron en la 0.2.0 y **todavía no los he visto pasar**. Localmente sí: la librería, los tests y un proyecto generado compilan con Clang 22. El de Docker es la única forma de verificar el Dockerfile —la máquina donde se escribió no alcanza los repos de Debian desde dentro de un contenedor—, pero hasta que el CI esté verde, tómalos como intención, no como hecho.

---

## Limitaciones conocidas

Las digo aquí en vez de que las descubras tú:

- **`.change()` de columnas solo en Postgres.** SQLite no tiene `ALTER COLUMN`: cambiar un tipo o una restricción exige reconstruir la tabla. Syrax lanza un error que lo explica en vez de generar SQL que el motor va a rechazar.
- **`syrax migrate` compila.** Las migraciones son C++, así que hay un build de por medio. Es el precio de que una migración pueda usar tus tipos y que un error de esquema lo atrape el compilador; con `ccache` la recompilación es de segundos.
- **Un middleware no ve el body *tipado*.** Corre antes del parseo: alcanza los bytes crudos por `request.drogon()->getBody()`, pero no el struct ya validado. Para reglas que dependen del contenido está `rules()`.
- **`Room` es de un solo proceso.** Un broadcast alcanza a las conexiones de *esta* instancia. Con varias réplicas detrás de un balanceador hace falta un bus externo, que Syrax no trae.
- **Sin colas ni cache.** Son [no-objetivos](#no-objetivos) deliberados, no pendientes.
- **Un proyecto generado no trae tests.** `syrax new` crea `database/factories/` pero ningún target de test ni `enable_testing()`. El framework sí está cubierto; tu proyecto tienes que montarlo tú por ahora.
- **Pre-1.0.** La API puede cambiar sin aviso.

## No-objetivos

```
ORM propio          Colas / Jobs        Scheduler
Query builder       Event bus           Service discovery
Relaciones          gRPC                Load balancing
Mail                Storage / S3        Circuit breakers
                    Cache distribuida   Broker de sockets
```

Syrax compone; no reemplaza. Si tu proyecto necesita algo de esto, tómalo de una librería existente.

**Regla de admisión:** una feature entra sólo si hace *notablemente más fácil crear una API*, y si es superficie finita. Un schema builder es finito (11 tipos de columna por 2 dialectos). Un ORM no lo es.

---

## Cómo funciona

```
┌──────────────────────────────────────────────────┐
│  Tu aplicación                                   │
├──────────────────────────────────────────────────┤
│  SYRAX  ← ~2700 lineas de cabeceras              │
│                                                  │
│  Ruteo con deducción de tipos desde la firma     │
│  Binding request → struct, y reglas por campo    │
│  Errores → respuesta JSON uniforme               │
│  Mapeo fila de BD → struct, por reflection       │
│  Schema builder y migraciones                    │
│  Generación de OpenAPI                           │
│  Middleware, JWT, políticas                      │
│  WebSockets con broadcast                        │
├─────────────────────────┬────────────────────────┤
│  GLAZE                  │  DROGON                │
│  reflection, JSON,      │  HTTP, async,          │
│  JSON Schema            │  corrutinas, BD        │
└─────────────────────────┴────────────────────────┘
```

Syrax escribe muy poco código. Las partes difíciles ya existen, son excelentes, y las mantiene alguien más.

El pegamento entre ellas no existía. Eso es Syrax.

---

## Licencia

MIT
