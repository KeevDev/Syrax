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

> **Estado: funcional, pre-1.0.** El CRUD, las migraciones, la base de datos y OpenAPI funcionan y están cubiertos por tests. La API puede cambiar sin aviso hasta la 1.0.

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

Requiere CMake 3.25+, git y un compilador con C++23 (GCC 14+ o Clang 17+).

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

### Validación estructural automática

El body se parsea en modo estricto antes de que el handler se ejecute. Si algo no cuadra, el cliente recibe un **422** y el handler nunca corre:

| | |
|---|---|
| Campo faltante | `missing_key` |
| Campo desconocido | `unknown_key` |
| Tipo incorrecto | `parse_number_failure` |
| JSON malformado | posición exacta del error |

> Esto valida la **forma** del JSON, no el contenido. Constraints por campo (largo mínimo, formato de email, rangos) todavía no existen — ver [Limitaciones](#limitaciones-conocidas).

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

### OpenAPI automático

`/openapi.json` y `/docs` con Swagger UI, generados de las rutas registradas. **Los esquemas salen de los mismos tipos que usan los handlers**, así que la documentación no puede desincronizarse del código: no hay anotaciones que mantener.

```cpp
app.docs("Mi API", "2.0.0");   // titulo y version
app.withoutDocs();             // apagarlo en produccion
```

---

## Estructura de un proyecto

```
config/app.json           ajustes del servidor (versionado)
.env                      credenciales (NO versionado)
docker/Dockerfile         imagen multi-etapa
public/                   estaticos
logs/

database/
├── migrations.cpp        que migraciones existen y en que orden
├── migrations/           las migraciones
├── seeders/              datos de ejemplo
└── factories/            objetos de mentira para tests

src/
├── main.cpp
├── bootstrap/            preparacion de la app
├── routes/               el mapa de la API, versionable
│   ├── routes.cpp
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
| `syrax test` | `t` | compila y corre los tests |
| `syrax upgrade` | `-u` | recompila e instala la última versión |
| `syrax version` | `-v` | versión y origen |

---

## Tests

```bash
syrax test                     # en el repo de Syrax
ctest --test-dir build         # equivalente
```

47 casos cubriendo el generador de DDL en ambos dialectos, `ALTER TABLE` ejecutado contra SQLite real, el mapeo de filas a structs, la generación de OpenAPI, y la integración HTTP completa: ruteo, binding de body, errores de validación, path params, corrutinas y el 404 en JSON.

CI en GitHub Actions compila y corre la suite en cada push y PR.

---

## Limitaciones conocidas

Las digo aquí en vez de que las descubras tú:

- **Sin constraints por campo.** La validación es estructural (forma del JSON), no semántica. `MinLen`, `Email`, `Range` están planeados pero no existen.
- **Sin middleware.** Ni autenticación, ni CORS, ni rate limiting.
- **El Dockerfile no se ha construido end-to-end.** Los nombres de paquete se verificaron contra packages.debian.org, pero en la máquina donde se escribió los contenedores no alcanzan los repos de Debian.
- **`syrax migrate` compila.** Las migraciones son C++, así que hay un build de por medio.
- **Sin ALTER de constraints.** Se pueden agregar, renombrar y quitar columnas, pero no cambiar el tipo ni las restricciones de una existente. Usa `Schema::raw()`.
- **Sin WebSockets, ni colas, ni cache.**

---

## No-objetivos

```
ORM propio          Colas / Jobs        Scheduler
Query builder       Event bus           Service discovery
Relaciones          gRPC                Load balancing
Mail                Storage / S3        Circuit breakers
```

Syrax compone; no reemplaza. Si tu proyecto necesita algo de esto, tómalo de una librería existente.

**Regla de admisión:** una feature entra sólo si hace *notablemente más fácil crear una API*, y si es superficie finita. Un schema builder es finito (12 tipos de columna por 2 dialectos). Un ORM no lo es.

---

## Cómo funciona

```
┌──────────────────────────────────────────────────┐
│  Tu aplicación                                   │
├──────────────────────────────────────────────────┤
│  SYRAX  ← ~1200 lineas                           │
│                                                  │
│  Ruteo con deducción de tipos desde la firma     │
│  Binding request → struct, con validación        │
│  Errores → respuesta JSON uniforme               │
│  Mapeo fila de BD → struct, por reflection       │
│  Schema builder y migraciones                    │
│  Generación de OpenAPI                           │
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
