# Syrax
<img width="1024" height="205" alt="image" src="https://github.com/user-attachments/assets/10dd0bb6-a67f-4460-b5c8-ecc87ad57838" />


**Un framework de APIs para C++ moderno.** Construido sobre [Drogon](https://github.com/drogonframework/drogon) (HTTP) y [Glaze](https://github.com/stephenberry/glaze) (tipos y JSON).

```cpp
// routes/v1.cpp — que expone la API
api.post("/users", user::store).as("users.store");
```

```cpp
// http/controllers/User/UserController.cpp — que hace la accion
Task<Result<resources::UserResource>> store(requests::CreateUser body) {
    const auto user = co_await service::create(std::move(body));
    if (!user) co_return Conflict("email already registered");

    co_return resources::from(*user);
}
```

Eso es un endpoint completo: parseo del body, validación, manejo de errores, serialización de la respuesta y documentación OpenAPI. Sin macros, sin heredar de nada, sin anotaciones.

De ahí para adentro hay un servicio y un repositorio, cada uno con su trabajo: el recorrido entero está en [el camino de una petición](#el-camino-de-una-petición).

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

### El camino de una petición

Un endpoint no es un archivo: es una cadena de capas, cada una con un trabajo y un tipo distinto. Esto es `POST /users` entero, de la URL a la tabla y de vuelta.

```
POST /users   {"name":"Ada","email":"ada@example.com","age":36}
      │
      ├─ routes/v1.cpp           la URL, y que metodo la atiende
      ├─ requests/CreateUser     se parsea y se valida  ──>  422, y aqui se acaba
      ├─ controllers/User        elige el codigo HTTP
      ├─ services/UserService    la regla de negocio
      ├─ repositories/User       el SQL  ──>  models::User
      └─ resources/UserResource  lo que sale, sin campos privados
      │
      ▼
201   {"id":1,"name":"Ada","email":"ada@example.com"}
```

Cada paso, con su archivo.

**1. La ruta** dice qué expone la API y quién la atiende. Nada más.

```cpp
// src/routes/v1.cpp
void register_(syrax::Group api) {
    api.get("/users", user::index).as("users.index");
    api.post("/users", user::store).as("users.store");

    api.get("/users/{id}", user::show).as("users.show");
    api.put("/users/{id}", user::update).as("users.update");
    api.del("/users/{id}", user::destroy).as("users.destroy");
}
```

**2. El request** es lo que entra, y declara sus propias reglas. Si el JSON no cuadra, el cliente recibe un **422** y el controlador **no llega a correr**.

```cpp
// src/http/requests/User/UserRequests.hpp
struct CreateUser {
    std::string name;
    std::string email;
    int         age;

    static auto rules() {
        return syrax::rules(
            syrax::field(&CreateUser::name).notEmpty().minLen(2).maxLen(80),
            syrax::field(&CreateUser::email).email(),
            syrax::field(&CreateUser::age).range(0, 130));
    }
};
```

**3. El controlador** es la única capa que habla HTTP. Llama al servicio y traduce lo que recibe a un código de estado; no tiene lógica propia.

```cpp
// src/http/controllers/User/UserController.cpp
Task<Result<resources::UserResource>> store(requests::CreateUser body) {
    const auto user = co_await service::create(std::move(body));
    if (!user) co_return Conflict("email already registered");

    co_return resources::from(*user);
}

Task<Result<resources::UserResource>> show(std::int64_t id) {
    const auto user = co_await service::byId(id);
    if (!user) co_return NotFound("user not found");

    co_return resources::from(*user);
}
```

**La firma es el contrato.** De ahí saca Syrax qué parsear, qué validar y qué documentar: sin argumentos, con path param tipado, con body, o con los dos —**el body siempre al final**—. Pueden ser síncronos (`Result<T>`) o corrutinas (`Task<Result<T>>`).

**4. El servicio** es la regla de negocio, y no sabe que existe HTTP: devuelve un `optional`, no un 404.

```cpp
// src/services/User/UserService.cpp
Task<std::optional<models::User>> create(requests::CreateUser input) {
    co_return co_await repo::createIfEmailFree(std::move(input.name),
                                               std::move(input.email), input.age);
}
```

Esa es la razón de que exista: el `nullopt` de arriba puede significar 409 en una API y otra cosa en un comando de consola. Quien decide es el controlador.

**5. El repositorio** es el único que toca la base. SQL a mano o query builder, como prefieras:

```cpp
// src/repositories/User/UserRepository.cpp
Task<std::vector<models::User>> all() {
    co_return co_await syrax::Query<models::User>().orderBy(&models::User::id).get();
}

Task<std::optional<models::User>> createIfEmailFree(std::string name, std::string email, int age) {
    co_return co_await syrax::db::transaction(
        [=](const syrax::db::Tx& tx) -> Task<std::optional<models::User>> {
            if (co_await syrax::Query<models::User>(tx.client())
                    .where(&models::User::email, "=", email).exists()) {
                co_return std::nullopt;
            }

            models::User nuevo{.id = 0, .name = name, .email = email, .age = age};
            co_await syrax::save(nuevo, tx.client());
            co_return nuevo;
        });
}
```

**6. El modelo y el resource** son dos tipos distintos a propósito: uno es la tabla, el otro es lo que sale por el cable.

```cpp
// src/models/User/User.hpp
struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;

    static constexpr auto table = "users";
};
```

```cpp
// src/http/resources/User/UserResource.hpp
struct UserResource {
    std::int64_t id;
    std::string  name;
    std::string  email;          // sin age, y sin passwordHash el dia que lo agregues
};
```

Un campo privado no puede filtrarse por accidente, porque el tipo que se serializa simplemente no lo tiene.

---

Así queda la cadena completa, y **ninguno de esos nombres los conoce el framework**: son archivos y funciones de C++ normales que puedes renombrar o borrar.

```
routes/  ->  http/controllers/  ->  services/  ->  repositories/  ->  models/
la URL       el transporte          el negocio     el SQL            la tabla
```

`syrax new` lo deja montado con este CRUD dentro, listo para copiar y seguir; el porqué de cada frontera está en [estructura de un proyecto](#estructura-de-un-proyecto).

**Para agregar un endpoint nuevo** el orden es el mismo de arriba: la ruta, el request si entra JSON, el método en el controlador, y hacia adentro solo lo que falte. Muchas veces el servicio ya sabe hacer lo que necesitas y no hay que bajar hasta el repositorio.

**Para algo de una línea, la lambda sigue valiendo.** Un `/health` no necesita una cadena de seis capas:

```cpp
app.get("/health", []() -> Result<Status> { return Status{.status = "ok"}; });
```

Es la misma maquinaria: lo que Syrax mira es la firma, no dónde esté escrita.

#### Generar las capas

Escribir seis archivos a mano en el orden correcto es lo que hace que, al tercer endpoint, alguien meta la query en el controlador. El CLI las escribe:

```bash
syrax make:api Post
```

```
creado   src/models/Post/Post.hpp
creado   src/http/requests/Post/PostRequests.hpp
creado   src/http/resources/Post/PostResource.hpp
creado   src/http/resources/Post/PostResource.cpp
creado   src/repositories/Post/PostRepository.hpp
creado   src/repositories/Post/PostRepository.cpp
creado   src/services/Post/PostService.hpp
creado   src/services/Post/PostService.cpp
creado   src/http/controllers/Post/PostController.hpp
creado   src/http/controllers/Post/PostController.cpp

falta la ruta. En src/routes/v1.cpp:
  ...
```

La ruta no se toca sola a propósito: es el único archivo donde decides qué se expone y bajo qué alias, y un generador que edita eso por su cuenta acaba peleándose contigo. Te deja las líneas listas para pegar.

Si sólo quieres una capa, `make:controller`, `make:service`, `make:repository`, `make:entity`, `make:request` y `make:resource` hacen exactamente esa. Y los dos que sí se registran solos, porque olvidarlo es el fallo clásico:

```bash
syrax make:job SendInvoice       # y queda en bootstrap/queue.cpp
syrax make:migration Post        # y queda en database/migrations.cpp
```

Un job sin registrar no corre nunca, y el síntoma es una cola que crece en silencio.

Para ver lo que hay montado, sin levantar el servidor:

```bash
syrax routes
```

```
GET     /api/v1/posts                   posts.index
POST    /api/v1/posts                   posts.store
DELETE  /api/v1/posts/{id}              posts.destroy
GET     /api/v1/posts/{id}              posts.show
PUT     /api/v1/posts/{id}              posts.update
```

### Errores como valores

```cpp
if (!user) co_return NotFound("user not found");
```

`BadRequest`, `Unauthorized`, `Forbidden`, `NotFound`, `Conflict`, `Unprocessable`, `Internal`. Todos se traducen a una respuesta JSON uniforme:

```json
{ "error": { "status": 404, "message": "user not found" } }
```

Incluso las rutas inexistentes responden JSON, no la página HTML de Drogon.

#### Tus propios errores

Un `Error` puede llevar un **código estable** además del mensaje. El mensaje se reescribe cuando alguien lo mejora; el código es sobre lo que el cliente ramifica:

```cpp
return Conflict("el email ya existe").as("email_duplicado");
```

```json
{ "error": { "status": 409, "message": "el email ya existe", "code": "email_duplicado" } }
```

`.explain("...")` añade una versión larga para un humano. Los dos campos son opcionales y sólo aparecen cuando los llenas, así que un cliente que hoy lee `error.message` sigue leyendo lo mismo mañana.

Con eso, el catálogo de errores de tu proyecto son funciones que devuelven `Error` y ya:

```cpp
namespace errors {
inline syrax::Error saldoInsuficiente(double falta) {
    return syrax::Conflict("saldo insuficiente")
        .as("saldo_insuficiente")
        .explain(std::format("faltan {:.2f}", falta));
}
}
```

#### Cambiar el formato

Si tu frontend ya espera otra forma, sustitúyela una vez:

```cpp
syrax::onError([](const syrax::Error& e) {
    Json::Value body;
    body["ok"]      = false;
    body["code"]    = e.code.empty() ? std::to_string(e.status) : e.code;
    body["message"] = e.message;
    return syrax::jsonResponse(body, e.status);
});
```

**El 422 de validación pasa por el mismo gancho**, a propósito: cambiar el formato y quedarte con dos formas de error distintas en la misma API sería peor que no poder cambiarlo. Las cabeceras de seguridad y CORS se siguen aplicando después.

#### Traducir excepciones

Una excepción que se escapa del handler es un 500 con cuerpo JSON y el `what()` **en el log, no en la respuesta** — decía cosas como `no such table: users`, que le describe el esquema a cualquiera que provoque el error.

Lo que tu proyecto sí sabe es qué excepciones merecen otro estado:

```cpp
syrax::onException([](const std::exception& e) -> std::optional<syrax::Error> {
    if (dynamic_cast<const drogon::orm::UniqueViolation*>(&e))
        return syrax::Conflict("el recurso ya existe").as("duplicado");
    return std::nullopt;
});
```

`std::nullopt` significa «ésta no la entiendo»: se queda como el 500 de siempre. Así traduces lo que conoces sin reimplementar el caso por defecto.

> Si sobrescribes el formato, `e.what()` queda a tu alcance. No lo pongas en la respuesta.

### Validación

El body se parsea en modo estricto antes de que el controlador se ejecute. Si algo no cuadra, el cliente recibe un **422** y el controlador nunca corre:

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

### Base de datos sin modelos generados

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

Conservas de Drogon el pool de conexiones, las corrutinas y los prepared statements. Así escrito, el SQL está a la vista; si prefieres no repetir la lista de columnas, hay un [query builder tipado](#query-builder-tipado) sobre el mismo struct.

**Los tres motores del ORM de Drogon.** `DB_ENGINE` acepta `postgres`, `mysql` (o `mariadb`) y `sqlite`, que son todos los que Drogon habla. Lo que cambia entre ellos lo absorbe el framework: el query builder numera los parámetros en Postgres (`$1`) y los deja posicionales (`?`) en los otros dos, y las migraciones escriben el DDL de cada uno. Drogon compila el soporte de un motor solo si encontró su librería de cliente, así que pedir uno que no está da un error con nombre al arrancar —qué instalar— en vez de un `LOG_FATAL` a media ejecución.

Para un valor suelto no hace falta declarar un struct:

```cpp
const auto total = co_await db::scalar<std::int64_t>("SELECT count(*) FROM users");
```

**Transacciones.** Un "comprobar y luego insertar" en dos consultas sueltas es una condición de carrera — dos peticiones ven el hueco libre las dos:

```cpp
co_return co_await db::transaction(
    [=](const db::Tx& tx) -> Task<std::optional<User>> {
        const auto taken = co_await tx.findOne<User>("SELECT ... WHERE email = $1", email);
        if (taken) co_return std::nullopt;

        co_return co_await tx.returning<User>("INSERT ... RETURNING ...", name, email, age);
    });
```

**El query builder también entra en la transacción.** El `Transaction` de Drogon *es* un `DbClient`, así que `tx.client()` sirve donde sea que se acepte un cliente:

```cpp
co_return co_await db::transaction(
    [=](const db::Tx& tx) -> Task<std::optional<User>> {
        if (co_await Query<User>(tx.client()).where(&User::email, "=", email).exists())
            co_return std::nullopt;

        User nuevo{.name = name, .email = email, .age = age};
        co_await save(nuevo, tx.client());
        co_return nuevo;
    });
```

`Tx` tiene las mismas cuatro operaciones que `db`. Si el cuerpo lanza, se deshace entera antes de propagar; `tx.rollback()` aborta sin lanzar, para cuando abortar es una decisión de negocio. Es lo que usa el repositorio que genera `syrax new`.

**El COMMIT se espera.** Drogon confirma la transacción al destruirla, en otro hilo: si falla —un deadlock, un *serialization failure*, una clave ajena diferida— eso ocurre **después** de que tu controlador devolvió el 201, y nadie se entera. Syrax espera esa confirmación y lanza `db::CommitFailed` si no llegó, así que un COMMIT roto sale por donde salen los demás errores y no como una fila que no está.

#### Query builder tipado

Para lo de todos los días —filtrar, ordenar, paginar, guardar— escribir el SQL a mano es repetir la lista de columnas en cuatro sitios y que una errata en `emial` la descubra producción. `Query<T>` cubre ese caso, y sólo ese. El modelo declara su tabla y ya:

```cpp
struct User {
    std::int64_t id;
    std::string  name;
    std::string  email;
    int          age;

    static constexpr auto table = "users";   // esto es todo lo que hace falta
};
```

```cpp
const auto adultos = co_await Query<User>()
    .where(&User::age, ">", 18)
    .orderBy(&User::name)
    .limit(10)
    .get();

const auto ada   = co_await Query<User>().where(&User::email, "=", email).first();
const auto total = co_await Query<User>().count();
const bool hay   = co_await Query<User>().whereNotNull(&User::email).exists();
const auto fuera = co_await Query<User>().where(&User::age, "<", 18).del();
```

También `orWhere`, `whereIn`, `whereNull`, `offset`. `toSql()` devuelve la consulta sin ejecutarla, para cuando quieras ver qué salió.

**Los `OR` van entre paréntesis o no van.** En SQL `AND` aprieta más que `OR`, así que una cadena plana de `where`/`orWhere` no significa lo que se lee de izquierda a derecha. `whereGroup` cierra el grupo:

```cpp
co_await Query<Invoice>()
    .where(&Invoice::status, "=", Status::Pending)
    .whereGroup([](auto& g) {
        g.where(&Invoice::total, ">", 100).orWhere(&Invoice::vip, "=", true);
    })
    .get();
// WHERE "status" = $1 AND ("total" > $2 OR "vip" = $3)
```

**Cambiar muchas filas en una sola consulta** —el hermano de `del()`, para no traerse cada objeto y guardarlo uno a uno:

```cpp
const auto cambiadas = co_await Query<Invoice>()
    .where(&Invoice::status, "=", Status::PendingPayment)
    .set(&Invoice::status, Status::Paid)
    .update();                      // devuelve cuántas tocó
```

Guardar y borrar objetos:

```cpp
User u{.name = "Ada", .email = "ada@x.com", .age = 36};
co_await save(u);      // INSERT; u.id queda con el que asignó la base
u.age = 37;
co_await save(u);      // UPDATE, porque la clave primaria ya viene puesta
co_await remove(u);
```

La clave primaria es `id` salvo que declares otra: `static constexpr auto primaryKey = "doc_id";`.

Cuatro cosas que lo separan de escribir el `SELECT`:

- **La columna se nombra con `&User::email`, no con un string.** Glaze resuelve el nombre en compilación comparando la dirección del miembro contra los campos que refleja, así que `&User::emial` **no compila** en vez de fallar en producción.
- **Las columnas salen del struct.** Añadir un campo al modelo no obliga a tocar ningún `SELECT`.
- **El dialecto lo pone Syrax.** Postgres numera los parámetros (`$1, $2`) y SQLite usa `?`; el motor sale del `.env`, así que la misma consulta vale en los dos.
- **Un `enum class` vale como valor y como campo.** La columna guarda el entero y el struct guarda los nombres: `where(&Invoice::status, "=", Status::Paid)` enlaza el tipo subyacente, y al leer la fila vuelve a ser `Status`.

**Lo que no hace, a propósito: joins, relaciones, subconsultas, `GROUP BY`.** Ahí un query builder deja de tener fondo y acaba siendo un dialecto de SQL peor que SQL. Para eso `db::query` sigue donde estaba, y las dos formas conviven en el mismo repositorio —de hecho el que genera `syrax new` usa una para lo simple y la otra para lo que no lo es.

#### Y si quieres `Mapper<T>`, puedes

Syrax usa `orm_lib` de Drogon entero —pool, corrutinas, transacciones, prepared statements— salvo `Mapper<T>`. Pero **no te lo impide**: corre sobre el mismo `DbClient`, así que convive en el mismo proyecto y en la misma transacción.

```bash
syrax make:model users     # escribe model.json con lo que hay en tu .env
```

Deja el modelo en `src/models/generated/`. La primera vez construye `drogon_ctl` desde el Drogon que ya bajó FetchContent —tarda varios minutos, pero queda cacheado en `build/_ctl/`— y necesita la base levantada, porque el esquema lo lee de ella. Por eso no se hace en `syrax new`: ahí todavía no hay base.

```cpp
#include <drogon/orm/CoroMapper.h>
#include "models/Users.h"

CoroMapper<drogon_model::api::Users> mapper(syrax::db::client());

const auto total = co_await mapper.count();
const auto page  = co_await mapper.orderBy(Users::Cols::_id).limit(3).findAll();
const auto uno   = co_await mapper.findByPrimaryKey(1);
```

Lo bueno que te llevas: **nombres de columna tipados** (`Users::Cols::_id` con una errata no compila) y `Criteria` para filtros compuestos.

Lo que cuesta, medido sobre una tabla `users` de 6 columnas generada con `drogon_ctl`:

| | líneas |
|---|---|
| `models/Users.h` + `Users.cc` | **1.632**, generadas |
| el `struct User` equivalente | **15**, escritas |

Unas 270 líneas por columna. Además cada campo es `std::shared_ptr<T>` (`getValueOfId()` / `getId()`), hay tres setters por columna, y regenerar exige `drogon_ctl` con la base levantada en tiempo de build.

Para cuatro columnas no compensa. Para un dominio grande con filtros dinámicos, empieza a pagar. La decisión es tuya, tabla por tabla.

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

> **`.change()` no existe en SQLite.** Postgres y MySQL lo soportan —cada uno con su sintaxis, que Syrax escribe por ti—; SQLite únicamente tiene `RENAME`, `ADD COLUMN` y `DROP COLUMN`, así que cambiar un tipo exige reconstruir la tabla entera. Syrax lanza un error que lo dice y apunta a `Schema::raw()` en vez de generar SQL que el motor va a rechazar. Es una limitación de SQLite, no de Syrax.

### OpenAPI automático

`/openapi.json` y `/docs` con Swagger UI, generados de las rutas registradas. **Los esquemas salen de los mismos tipos que usan los controladores**, así que la documentación no puede desincronizarse del código: no hay anotaciones que mantener.

```cpp
app.docs("Mi API", "2.0.0");   // titulo y version
app.withoutDocs();             // apagarlo en produccion
```

Eso incluye los path params: un controlador que declara `show(std::int64_t id)` documenta `{id}` como `integer`, no como `string`. El tipo lo pone la firma, igual que el resto.

Y el documento se puede sacar sin levantar el servidor, para volcarlo en CI o generar clientes:

```cpp
const auto spec = app.openApi();   // lo mismo que sirve /openapi.json
```

### Recarga al guardar

`syrax serve` se queda de padre del servidor y vigila `src/` y `database/`. Guardas un `.cpp` y recompila y vuelve a levantar solo; `r` lo fuerza a mano y `q` sale.

**Si no compila, el servidor anterior sigue vivo.** Quedarte sin servidor justo cuando acabas de romper el código es lo contrario de lo que quieres: verás el error del compilador y el binario de antes seguirá respondiendo hasta que arregles.

Sin terminal interactiva —un contenedor, CI, una tubería— no hay teclado que escuchar y se comporta como siempre. `--no-watch` apaga la vigilancia.

### Configuración

La configuración de un proyecto vive en `src/bootstrap/`, un archivo por cosa que se configura, y son funciones C++ normales — no un formato que haya que aprender:

```
src/bootstrap/
├── app.cpp         junta todo: .env, base de la API, y llama a las de abajo
├── database.cpp    la conexión a la base
├── cache.cpp       el Redis, si lo enciendes
├── queue.cpp       el driver de la cola y los jobs registrados
├── middleware.cpp  CORS, rate limit y cabeceras de seguridad
└── errors.cpp      qué excepciones merecen otro estado
```

```cpp
// src/bootstrap/middleware.cpp
void middleware(syrax::App& app) {
    app.cors({
        .origins     = {syrax::env("CORS_ORIGINS", "*")},
        .credentials = syrax::envBool("CORS_CREDENTIALS", false),
    });

    app.useOnResponse(syrax::securityHeaders());

    app.use(syrax::rateLimit(syrax::envInt("RATE_LIMIT", 120), std::chrono::minutes{1}));
}
```

Los tres middlewares vienen puestos en todo proyecto nuevo. Antes existían y no los usaba nadie, porque no se veían desde ningún sitio.

```cpp
// src/bootstrap/database.cpp
void database() {
    syrax::db::connect({
        .engine      = syrax::env("DB_ENGINE", "postgres"),
        .host        = syrax::env("DB_HOST", "127.0.0.1"),
        .port        = static_cast<unsigned short>(syrax::envInt("DB_PORT", 0)),
        .database    = syrax::env("DB_NAME", "api"),
        .username    = syrax::env("DB_USER", "postgres"),
        .password    = syrax::env("DB_PASSWORD", "postgres"),
        .connections = static_cast<std::size_t>(syrax::envInt("DB_POOL", 4)),
    });
}
```

El valor por defecto está **al lado** de la clave, y se ve de un vistazo qué lee la app del entorno. `syrax::env`, `envInt` y `envBool` leen el proceso con respaldo al `.env`; lo que ya existe en el entorno gana siempre, para que el despliegue pueda pisar el archivo. Un valor mal escrito (`DB_PORT=cinco`) avisa por `stderr` en vez de caer en silencio al default.

Llamar varias veces a `connect()` con `name` distinto da varias conexiones; `db::client("informes")` pide una por nombre.

**El puerto** se resuelve de más a menos prioridad:

```bash
./mi-api 3000        # 1. argumento explícito
APP_PORT=3000        # 2. entorno, o el .env
                     # 3. 8080
```

**La base de la API** se declara una vez, en `bootstrap/app.cpp`, y las rutas cuelgan de ahí:

```cpp
app.base(syrax::env("API_BASE", "/api/v1"));   // en bootstrap
routes::v1::register_(app.api());              // en routes/routes.cpp
```

Cambiar `API_BASE` mueve la API entera sin tocar una sola ruta. Lo que no es de la API —un `/health`, los estáticos— se sigue registrando con su ruta completa: la base no es un prefijo global.

### Trazabilidad

Cada petición recibe un identificador corto, y con él se puede encontrar *esa* petición entre todas las demás. Va puesto de fábrica: no hay nada que encender.

```
201  POST   /api/v1/users                          12.4ms  7dw1ubha8o09
404  GET    /api/v1/users/9999                      0.8ms  k2p0zx4mq1te
500  POST   /api/v1/orders                         31.7ms  9a8sbd03nfl2
```

Eso es lo que ves en la terminal mientras desarrollas. Detrás de un pipe —un contenedor, el CI, un recolector de logs— la misma información sale como una línea JSON por evento, porque es lo que esas herramientas saben leer:

```json
{"ts":"2026-09-16T04:42:27.380Z","level":"info","msg":"request","method":"POST","path":"/users","status":"201","ms":"12.4","ip":"127.0.0.1","request_id":"7dw1ubha8o09"}
```

El id viaja en la cabecera `X-Request-Id` de la respuesta, así que quien reporta un fallo tiene algo que citar. Y si la petición **ya traía** una —porque la mandó un gateway u otro servicio—, se respeta: la traza cruza el salto entera.

Para tus propios eventos:

```cpp
syrax::log::info("pedido confirmado", {{"pedido", std::to_string(id)},
                                       {"request_id", syrax::log::requestId(request)}});
```

| Variable | Qué hace |
|---|---|
| `LOG_FORMAT` | `json` o `text`. Por defecto: texto si hay terminal, JSON si no. |
| `LOG_LEVEL` | `debug`, `info`, `warn`, `error`. Por defecto `info`. |
| `LOG_ACCESS` | `0` apaga la línea por petición. |

### Cache

Redis, configurado igual que la base y apagado hasta que lo enciendas:

```cpp
// src/bootstrap/cache.cpp
void cache() {
    if (!syrax::envBool("CACHE_ENABLED", false)) return;

    syrax::cache::connect({
        .host = syrax::env("REDIS_HOST", "127.0.0.1"),
        .port = static_cast<unsigned short>(syrax::envInt("REDIS_PORT", 6379)),
    });
}
```

```cpp
co_await cache::put("users.total", "42", std::chrono::minutes{10});
const auto total = co_await cache::get("users.total");   // optional<string>
co_await cache::forget("users.total");
```

Y el patrón de siempre, que además serializa por ti:

```cpp
co_return co_await cache::remember<std::vector<User>>(
    "users.activos", std::chrono::minutes{10},
    [] { return repo::activos(); });
```

El valor viaja como JSON, así que sirve para cualquier struct que el resto del framework ya sabe serializar. Si el JSON guardado ya no encaja con el tipo —cambiaste el struct— se trata como un fallo de cache: se recalcula y se pisa, en vez de reventar.

`cache::client()` da el `RedisClient` de Drogon para todo lo demás. Pedirlo antes de `app.run()` lanza con un mensaje en vez de un segfault: Drogon crea sus clientes al arrancar.

### Colas de trabajos

Mandar un correo, rehacer un informe o llamar a una API ajena no tiene por qué pasar dentro de la petición. Un job es un struct plano con `handle()`:

```cpp
// src/jobs/SendWelcome.hpp
struct SendWelcome {
    std::int64_t userId = 0;
    std::string  email;

    static constexpr auto name  = "send-welcome";
    static constexpr int  tries = 3;

    syrax::Task<void> handle() const {
        co_await mail::send(email, "Bienvenida");
    }
};
```

Lo que viaja por la cola es su JSON, que Glaze ya sabe escribir y leer: no hay nada que declarar. Se despacha desde donde haga falta —normalmente el servicio— y se ejecuta después:

```cpp
co_await jobs::dispatch(SendWelcome{.userId = user->id, .email = user->email});
co_await jobs::dispatch(SendWelcome{...}, jobs::in(std::chrono::minutes{5}));
co_await jobs::dispatch(SendWelcome{...}, jobs::on("correos"));
```

El registro va en `bootstrap/queue.cpp`, junto al resto de la configuración:

```cpp
syrax::jobs::handle<SendWelcome>();
```

No es burocracia: el worker recibe texto, y C++ no tiene reflection en ejecución. Esa línea es lo que le permite volver del nombre `"send-welcome"` al tipo.

**El worker es el mismo binario**, con otro argumento:

```bash
syrax queue:work      # corre los jobs encolados
syrax queue:failed    # los que se rindieron, con el motivo
syrax queue:retry     # devuelve los fallidos a la cola
```

```
$ syrax queue:work
  send-welcome (intento 1)  bienvenida para ada@example.com (#6)  ok
```

Levanta el loop de Drogon sin escuchar en ningún puerto, así que un job usa la base, el cache y tus servicios **exactamente igual** que un controlador. No hay un segundo mundo que mantener al día.

**Dos drivers**, y la diferencia importa:

| | |
|---|---|
| `database` | La misma base que el resto de la app. Una tabla, ninguna infraestructura nueva, y **entra en tus transacciones**. |
| `redis` | Sin esquema, y más rápido cuando el volumen sube. Cada operación es un script Lua, así que reservar un job es atómico entre workers. |

Lo transaccional es lo que solo puede dar el primero:

```cpp
co_await db::transaction([&](const db::Tx& tx) -> Task<...> {
    co_await save(pedido, tx.client());
    co_await jobs::dispatch(CobrarPedido{pedido.id}, tx);   // el mismo COMMIT
});
```

Si el `COMMIT` falla, el job **tampoco existe**. Con Redis son dos sistemas distintos y nada los une: por eso ese `dispatch` con `tx` lanza si el driver no es el de base de datos, en vez de aparentar una garantía que no tiene.

**Reintentos.** `tries` dice cuántas veces; entre una y otra la espera crece, porque reintentar de inmediato contra algo que está caído solo gasta los intentos que quedan. Al agotarlos el job pasa a los fallidos con el motivo, y ahí espera a que alguien lo mire.

**Un job se ejecuta al menos una vez, no exactamente una vez.** Si el worker muere a mitad, el job sigue reservado hasta que vence `QUEUE_RETRY_AFTER` y entonces vuelve a la cola. Escríbelos de forma que correr dos veces no haga daño: es la misma regla que en cualquier otra cola.

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
// http/controllers/Post/PostController.cpp
Task<Result<PostResource>> update(Request req, std::int64_t id, UpdatePost body) {
    const auto actor = actorFrom(req);

    if (auto denied = requireRole(actor, "admin", "editor")) co_return *denied;
    // ...
}
```

`Request` es opcional y va primero; el body, último. Un controlador que no necesita la petición cruda no la declara.

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
docker-compose.yml        postgres o mysql, si elegiste uno de los dos
public/
├── index.html            portada: Syrax, y dos tarjetones
├── dragon.jpg            la ilustracion de la portada
└── syrax.html            el started del framework, sin salir del proyecto
logs/

database/
├── migrations.hpp        declara el registro
├── migrations.cpp        que migraciones existen y en que orden
├── migrations/           las migraciones
├── seeders/              datos de ejemplo
└── factories/            objetos de mentira para tests

src/
├── main.cpp
├── bootstrap/            configuracion: app.cpp, database.cpp, cache.cpp, queue.cpp
├── jobs/                 lo que corre fuera de la peticion
├── routes/               el mapa de la API, versionable
│   ├── routes.cpp        /health y el alta de cada version
│   └── v1.cpp            una linea por endpoint: ruta -> metodo del controlador, sin repetir el prefijo
├── http/                 TODO lo atado al transporte
│   ├── controllers/User/
│   ├── requests/User/
│   └── resources/User/
├── services/User/        logica de negocio
├── repositories/User/    SQL
└── models/User/          la forma de la tabla

tests/
├── CMakeLists.txt        Catch2, solo cuando corres `syrax test`
└── user_test.cpp         un test de verdad, para copiar y seguir
```

**Por qué la ruta no vive en el controlador:** `routes/v1.cpp` se lee de un vistazo y dice qué expone esta versión de la API; el controlador dice qué hace cada acción. Cómo se escriben las dos está arriba, en [el camino de una petición](#el-camino-de-una-petición); aquí solo falta de dónde sale el prefijo, que es de la configuración:

```cpp
// routes/routes.cpp — la base se declara en bootstrap, aqui solo se usa
routes::v1::register_(app.api());
```

**Grupos y alias.** `app.api()` es el grupo de la base; `group()` anida (`app.api().group("/admin")`), y `as()` le pone nombre a una ruta para no volver a escribirla:

```cpp
urlFor("users.show", 42)   // "/api/v1/users/42"
```

Sirve para la cabecera `Location` de un 201, o para enlazar un recurso desde otro. Un alias que nadie registró devuelve vacío, no una URL inventada.

**Por qué existe `http/`:** marca un límite real. Si mañana expones la misma lógica por gRPC, esa carpeta se tira entera y `services/`, `repositories/` y `models/` siguen sirviendo sin tocarse.

**Por qué `models/` y `resources/` están separados:** `User` tiene `passwordHash` y `UserResource` no. Un campo privado no puede filtrarse por accidente porque el tipo que se serializa simplemente no lo tiene.

Ninguno de esos nombres los conoce el framework: son archivos C++ normales. Renómbralos o bórralos.

---

### Tests en tu proyecto

`syrax new` deja `tests/` con Catch2 configurado y un test que ya prueba algo real: el mapeo a resource y las reglas del request.

```bash
syrax test        # baja Catch2 la primera vez, compila y corre ctest
```

Todo tu código menos `main.cpp` va a una librería (`<proyecto>_lib`) y el ejecutable solo enlaza contra ella. Es lo que permite que un test enlace tus servicios y repositorios: un ejecutable con `main` dentro no se puede enlazar dos veces.

Agregar un archivo a `tests/` no obliga a tocar ningún CMake, y `database/factories/` está ahí para construir objetos de mentira sin repetirte.

---

## Comandos

| | | |
|---|---|---|
| `syrax new <nombre>` | `n` | crea un proyecto (`--db postgres\|mysql\|sqlite`) |
| `syrax build` | `b` | configura y compila |
| `syrax serve` | `s` | levanta y **recompila al guardar** (`--port N`, `--no-watch`) |
| `syrax routes` | `r` | lista las rutas registradas, con su alias |
| `syrax migrate` | `m` | aplica las migraciones pendientes |
| `syrax migrate:rollback` | `m:r` | revierte la última |
| `syrax migrate:status` | `m:s` | muestra cuáles están aplicadas |
| `syrax db:seed` | `seed` | carga `database/seeders/*.sql` |
| `syrax make:model <tabla>` | `m:m` | genera el modelo de Drogon (`Mapper<T>`) desde la base |
| `syrax make:api <Nombre>` | `m:a` | las seis capas de un recurso, de una vez |
| `syrax make:controller\|service\|repository <Nombre>` | | una sola capa |
| `syrax make:entity\|request\|resource <Nombre>` | | una sola capa |
| `syrax make:job <Nombre>` | `m:j` | un job, **ya registrado** en `bootstrap/queue.cpp` |
| `syrax make:migration <Nombre>` | `m:mg` | una migración, **ya registrada** en `migrations.cpp` |
| `syrax queue:work` | `work` | corre los jobs encolados |
| `syrax queue:failed` | `q:f` | los que se rindieron, con el motivo |
| `syrax queue:retry` | `q:r` | devuelve los fallidos a la cola |
| `syrax test` | `t` | compila y corre `ctest` (ver nota abajo) |
| `syrax upgrade` | `-u` | recompila e instala la última versión |
| `syrax version` | `-v` | versión y origen |
| `syrax help` | `-h` | esta lista |

> **`syrax test` funciona igual en tu proyecto que en el repo de Syrax.** Baja Catch2, compila `tests/*.cpp` y corre `ctest`. Catch2 vive detrás de una opción (`SYRAX_PROJECT_TESTS`) que enciende ese comando, así que tu `syrax build` y tu `syrax serve` de todos los días no lo arrastran.

---

## Tests

```bash
syrax test                     # en el repo de Syrax
ctest --test-dir build         # equivalente
```

137 casos cubriendo el generador de DDL en ambos dialectos, `ALTER TABLE` ejecutado contra SQLite real, el mapeo de filas a structs, el query builder contra SQLite real —SQL generado, `save`/`remove`, `update` masivo, paginación, enums, el `IN ()` vacío y que agrupar con `whereGroup` cambia qué filas vuelven—, las reglas de validación y su anotación del JSON Schema, la generación de OpenAPI, JWT y hashing de contraseñas, middlewares y políticas, la integración HTTP completa (ruteo, binding de body, 422 con detalle por campo, path params, corrutinas, el 404 y el 500 en JSON) y los WebSockets hablando el protocolo a mano contra el servidor real.

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

- **`.change()` de columnas no va en SQLite.** El motor no tiene `ALTER COLUMN`: cambiar un tipo o una restricción exige reconstruir la tabla. Syrax lanza un error que lo explica en vez de generar SQL que el motor va a rechazar. En Postgres y MySQL funciona.
- **`syrax migrate` compila.** Las migraciones son C++, así que hay un build de por medio. Es el precio de que una migración pueda usar tus tipos y que un error de esquema lo atrape el compilador; con `ccache` la recompilación es de segundos.
- **Un middleware no ve el body *tipado*.** Corre antes del parseo: alcanza los bytes crudos por `request.drogon()->getBody()`, pero no el struct ya validado. Para reglas que dependen del contenido está `rules()`.
- **`Room` es de un solo proceso.** Un broadcast alcanza a las conexiones de *esta* instancia. Con varias réplicas detrás de un balanceador hace falta un bus externo, que Syrax no trae.
- **El cache es un Redis, no una capa de cache.** `cache::` configura el cliente de Drogon y le pone encima `get`/`put`/`forget`/`remember`. No hay drivers intercambiables, tags, ni invalidación por dependencias: para eso está el cliente crudo.
- **Un job corre al menos una vez.** Si el worker muere con uno en la mano, vuelve a la cola cuando vence `QUEUE_RETRY_AFTER`. No hay forma barata de prometer "exactamente una vez", así que Syrax no la promete.
- **Sin scheduler.** Un job se difiere (`jobs::in(...)`), pero no hay cron: para eso está cron.
- **`ccache` solo acierta si el directorio de build es el mismo.** FetchContent deja Drogon *dentro* de `build/`, así que sus rutas de include forman parte de cada compilación: dos directorios distintos son dos entradas distintas y la caché no sirve. Borrar y rehacer `build/` en el mismo sitio sí acierta al 100%. Con `CCACHE_BASEDIR` se puede sortear, pero eso es configuración tuya, no del proyecto.
- **Pre-1.0.** La API puede cambiar sin aviso.

## No-objetivos

```
Joins y relaciones  Scheduler / cron    Service discovery
Lazy / eager load   Event bus           Load balancing
Mail                gRPC                Circuit breakers
                    Storage / S3        Broker de sockets
                    Drivers de cache
```

Syrax compone; no reemplaza. Si tu proyecto necesita algo de esto, tómalo de una librería existente.

**Regla de admisión:** una feature entra sólo si hace *notablemente más fácil crear una API*, y si es superficie finita. Una cola lo es: encolar, sacar, ejecutar, reintentar, rendirse y diferir. Con cadenas, lotes y colas con rate limit deja de serlo, y por eso no están. Un schema builder es finito (11 tipos de columna por 3 dialectos). Un query builder sin joins también: filtrar, ordenar, paginar, guardar y borrar. Con relaciones —lazy loading, cascadas, el N+1— deja de serlo, y por eso `Query<T>` se planta justo ahí.

---

## Cómo funciona

```
┌──────────────────────────────────────────────────┐
│  Tu aplicación                                   │
├──────────────────────────────────────────────────┤
│  SYRAX  ← ~4200 lineas de cabeceras              │
│                                                  │
│  Ruteo con deducción de tipos desde la firma     │
│  Binding request → struct, y reglas por campo    │
│  Errores → respuesta JSON uniforme               │
│  Mapeo fila de BD → struct, por reflection       │
│  Query builder tipado sobre ese mismo struct     │
│  Schema builder y migraciones, en los 3 motores  │
│  Configuracion y cache en Redis                  │
│  Colas de trabajos, en base de datos o Redis     │
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
