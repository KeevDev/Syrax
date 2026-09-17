# Hacia dónde crece Syrax

Esta lista no es un backlog de deseos: es lo que pasa la **regla de admisión** del
README —*una feature entra sólo si hace notablemente más fácil crear una API, y si
su superficie es finita*— más lo que se queda fuera y por qué, que suele ser la
parte más útil de un roadmap.

Lo hecho se queda como registro de qué se cerró y de qué cambió de plan por el
camino; lo demás baja o sube de nivel según lo que se aprenda. Si algo lleva
mucho tiempo sin moverse, probablemente es que no pasaba la regla.

---

## Antes de pedir nada: esto ya está

La mitad de lo que uno quiere pedirle a un framework de APIs ya está en Syrax,
a veces con otro nombre. Conviene mirar aquí primero.

| Lo que se pide | Dónde está | Matiz |
|---|---|---|
| **Rate limiting** | `middleware.hpp` → `rateLimit()` y `rateLimitShared()` | El primero cuenta en el proceso; el segundo en Redis, para que el límite valga con varias instancias. Los dos aceptan por quién contar. |
| **CORS configurable** | `middleware.hpp` → `CorsOptions` | Ya es configurable: `origins`, `methods`, `headers`, `credentials`, `maxAge`, y desde el nivel 1 viene cableado en `bootstrap/middleware.cpp`. |
| **API Keys** | `middleware.hpp` → `requireApiKey()` | Con cabecera configurable. |
| **JWT** | `auth.hpp` → `sign`, `verify`, `bearer` | Y `hashPassword`/`verifyPassword` con PBKDF2, que suele faltar. |
| **Guards** | `middleware.hpp` + `policy.hpp` | Un guard es un middleware que devuelve `Error`. Es el mismo concepto con otro nombre. |
| **Policies / ABAC** | `policy.hpp` | Una policy *es* una función sobre actor + recurso: eso ya es ABAC, sin el vocabulario. |
| **RBAC** | `policy.hpp` → `requireRole(actor, "admin", "editor")` | Roles simples. Una tabla de permisos editable en runtime es aplicación, no framework. |
| **Scopes / permisos finos** | `policy.hpp` → `requireScope`, `requireAnyScope`, `Actor::can` | Formato de OAuth2, sin comodines. |
| **Soft deletes** | `query.hpp` → `softDeletes` en el modelo | Con `withTrashed`, `onlyTrashed`, `restore` y `forceDelete`. |
| **Auditoría** | `audit.hpp` → `record`, `of`, `by` | Append-only, y transaccional si se le pasa el cliente de la transacción. |
| **Cliente HTTP** | `http.hpp` → `http::Client` | Timeout, reintentos sólo idempotentes y propagación del request-id. |
| **OAuth2 / OIDC (validar)** | `jwks.hpp` → `auth::jwks(url)` | Tokens de Auth0, Keycloak o Cognito. Ser el proveedor sigue fuera. |
| **Subida de archivos** | `uploads.hpp` → `Uploads`, `upload()` | Firma de bytes para imágenes y pdf. El guardado no: eso es storage. |
| **Métricas** | `app.metrics()` | Cuatro contadores en formato Prometheus. |
| **Multi-tenancy por fila** | `Query<T>::forTenant(id)`, `syrax make:api X --tenant` | Olvidarlo lanza, no filtra. El CLI lo enhebra por todas las capas. |
| **DTOs** | `resources/` y `requests/` del andamiaje | Son DTOs de entrada y de salida, con el nombre que usa el README. |
| **Cache abstraction** | `cache.hpp` → `get/put/forget/has/remember` | La abstracción existe; el único driver es Redis. |
| **Healthcheck** | `health.hpp` → `app.health()` | Pregunta a cada base y cada Redis registrados. `health::probe()` agrega las del proyecto. |
| **Configuración tipada** | `config.hpp` → `config::load<T>()` | Del entorno por reflexión, validada al arrancar con las mismas `rules()` del resto. |
| **Connection pooling configurable** | `db::Connection::connections`, env `DB_POOL` | |
| **Multi-database** | `db::client("nombre")` + `queryOn` / `executeOn` / `transactionOn` | **Ya funciona hoy.** Lo que falta es azúcar en bootstrap y un apartado en el README, no código. |
| **Manejo de response JSON** | `Result<T>` + `makeOk` / `makeError` | El handler devuelve el valor o el `Error`; el borde lo traduce. |
| **Outbox pattern** | `jobs::dispatch(job, tx)` | Encolar dentro de la misma transacción que escribe la fila es el 80% del outbox, y es la parte que importa. |
| **Documentación OpenAPI** | `openapi.hpp` | Generada desde los tipos, no escrita a mano. |
| **Interfaces** | — | Cuando hay dos implementaciones reales, sí: `jobs::Driver` lo es. Con una sola es un vtable y un archivo de más. Ver *Inyección de dependencias*, abajo. |

---

## Hecho

Entregado y verificado sobre un proyecto generado. Queda aquí como registro de
qué se cerró; lo que sigue pendiente está en el nivel 2.

| Qué | Cómo quedó |
|---|---|
| **Capa de errores** | `errors.hpp`: `Error` gana `code` y `detail`, `onError` sustituye el formato (el 422 de validación incluido), `onException` traduce excepciones y `std::nullopt` deja el 500 de siempre. |
| **Trazabilidad** | `log.hpp`: request-id de fábrica, respetando el que venga de fuera; `X-Request-Id` en la respuesta; log estructurado con `LOG_FORMAT`/`LOG_LEVEL`/`LOG_ACCESS`. |
| **Requests en vivo** | Sale de lo anterior sin CLI de por medio: con terminal, una línea en color por petición; detrás de un pipe, JSON por línea. |
| **`bootstrap/middleware.cpp`** | CORS, rate limit y cabeceras de seguridad cableados en todo proyecto nuevo. Más `bootstrap/errors.cpp`. |
| **Generadores por capa** | `make:api` y las seis capas por separado, más `make:job` y `make:migration`, que además **se registran solos**. |
| **`syrax routes`** | Método, ruta y alias, sin levantar el servidor. |
| **Apagado ordenado** | `setTermSignalHandler`: el worker termina el job en curso y sale; un segundo Ctrl-C sale ya. |
| **Tests en paralelo** | El puerto de `tests/test_server.cpp` lo elige el kernel con `run(0)` y Drogon dice cuál tocó. `ctest -j8` pasa los 209; antes fallaban 13. |
| **Kit de tests** `syrax::testing` | `testing.hpp`: levanta la aplicación real del proyecto contra una sqlite temporal y le habla por TCP. `syrax new` deja un test que va ruta → controller → service → repositorio → SQL. |
| **`/health` de verdad** | `health.hpp` + `app.health()`: un `SELECT 1` por base y un `PING` por Redis registrados. 200 si responden, 503 con el desglose si no. `health::probe()` agrega las del proyecto. |
| **Configuración tipada** | `config.hpp`: un `struct Config` se llena del entorno por reflexión (`dbPool` lee `DB_POOL`) y se valida con las mismas `rules()` del resto. Falla al arrancar con el nombre de la variable y todos los problemas juntos. El andamiaje trae `bootstrap/config.cpp` y los demás `bootstrap/` leen de ahí. |
| **Rate limit con Redis** | `rateLimitShared()`, sobre una cadena de middleware asíncrona nueva (`useAsync`). Ventana fija con el índice en la clave; si Redis no responde, la petición pasa. Los dos limitadores aceptan una función de clave: por IP de fábrica, por cabecera o por usuario si se pide. |
| **ETag / 304** | `app.etag()`: SHA-256 truncado del cuerpo en cada GET de éxito, y 304 sin cuerpo si el `If-None-Match` coincide. |
| **CLI: `db`, `redis`, `cache:clear`** | Consolas con las credenciales del `.env` ya puestas, para los tres motores. `cache:clear` pregunta antes, porque `FLUSHDB` no distingue lo de Syrax de lo que haya puesto otra aplicación en el mismo Redis. |
| **Scheduler** | `schedule.hpp`: `every()`, `dailyAt("HH:MM")` y `hourlyAt(N)`. No ejecuta, **encola**, así que la tarea es un job con sus reintentos. Sin expresiones cron. `syrax schedule:work` y `schedule:list`. |
| **`Idempotency-Key`** | `idempotency.hpp` + `app.idempotency()`: el reintento devuelve la respuesta guardada. La misma clave con otro cuerpo es un 422, dos simultáneas son un 409, y un 5xx suelta la clave. |
| **Paginación estándar** | `Query<T>::paginate(page, per)` devuelve un `Page<T>` con `data`, `total`, `page`, `perPage`, `pages` y `hasMore`, y OpenAPI lo documenta sin que nadie lo escriba. |
| **Serialización parcial** | `app.partial()`: `?fields=id,name` recorta la respuesta. En una página filtra dentro de `data` y deja el sobre entero. Un `fields` con sólo nombres inventados es un 400, no objetos vacíos. |
| **Scopes** | `requireScope(actor, "pedidos:escribir")` exige TODOS; `requireAnyScope` se conforma con uno. El claim `scope` viaja como lo define OAuth2, así que un token de un tercero ya encaja. |
| **Soft deletes y timestamps** | `static constexpr auto softDeletes` / `timestamps` en el modelo. `del()` marca en vez de borrar, `withTrashed()` / `onlyTrashed()` / `restore()` / `forceDelete()`, y `created_at`/`updated_at` los pone la base. |
| **Auditoría** | `audit.hpp`: tabla append-only con quién, qué, sobre qué y el request-id, que sale de la trazabilidad sin copiarlo a mano. Dentro de la transacción que hace el cambio, para que no registre cosas que no pasaron. |
| **Cliente HTTP** | `http.hpp`: timeout de fábrica, reintento con espera creciente **sólo en métodos idempotentes**, y `trace(request)` que propaga el `X-Request-Id`. Sin breaker ni descubrimiento. |
| **Validar tokens de terceros** | `jwks.hpp` → `auth::jwks(url)`: firma RS256 contra el JWKS del proveedor, con caché de claves y recarga limitada. El algoritmo **se exige**, no se lee del token; `issuer` y `audience` se comprueban. |
| **Subida de archivos con reglas** | `uploads.hpp`: tamaño, extensión y —lo que importa— `image()`/`pdf()` que miran los **primeros bytes**, no el tipo que declara el cliente. No hay filtro por MIME, y el motivo está escrito. |
| **Métricas** | `app.metrics()`: cuatro contadores en el formato de Prometheus. Sin etiquetas por ruta, porque una ruta con un id dentro es cardinalidad sin techo. |
| **Multi-tenancy por fila** | `static constexpr auto tenant` + `Query<T>::forTenant(id)`. Olvidarlo **lanza** en vez de devolver las filas de todos: un fallo ruidoso en vez de una fuga silenciosa. |
| **`syrax new` con asistente** | Dos ejes además del motor —cache y auth—, los que cambian archivos de verdad. La pregunta de `/docs` se quedó fuera: cambia una línea y no vale duplicar la matriz. |

Tres cosas que cambiaron de plan por el camino:

- **No hay `bootstrap/logging.cpp`.** Las tres variables de entorno ya configuran
  el log, y un archivo que sólo las relee es ruido en un directorio donde cada
  archivo debería justificar su sitio.
- **El kit de tests no redirige la base con un gancho nuevo.** La idea era un
  `db::use()` que el test pudiera pisar. No hizo falta: `loadDotEnv()` escribe
  con `overwrite=0`, así que poner `DB_ENGINE` y `DB_FILE` en el entorno antes
  de llamar a `create()` ya le gana al `.env`, y el proyecto conecta a la sqlite
  temporal por su camino de siempre. Una costura menos que mantener.
- **La vista en vivo no la pinta el CLI.** Parsear el stdout del hijo era la idea
  original; el framework ya sabe si está delante de una terminal, así que la
  línea en color la escribe él. Menos piezas y el mismo resultado.

## Nivel 2 — vacío

No queda nada. Lo que había está arriba, en *Hecho*; lo que se descartó y por
qué, abajo. Cuando aparezca algo nuevo que pase la regla de admisión, aquí es
donde va antes de escribirse.

## Nivel 3 — lo que falta, y por qué sale caro

**Todo lo de aquí es objetivo.** Ninguna es un "nunca": el plan es un framework
completo, y a la larga uno que no dependa de otro. Lo que esta lista dice no es
qué se descarta, sino **qué hay que resolver antes** de que cada cosa pueda
entrar sin quedarse a medias.

Agrupadas por lo que las hace caras, que es más útil que una lista plana y es lo
que decide el orden.

El precedente está arriba, en *Hecho*: las **tareas periódicas** vivieron en
este nivel hasta que existieron las colas —entonces ya había dónde poner el
trabajo y cupieron en veinte líneas—, y los **joins** y las **relaciones**
salieron de aquí en cuanto se vio dónde plantarse.

### Depende de algo que el proceso no controla

| Qué | Qué hay que resolver antes |
|---|---|
| **API Gateway**, **Service Discovery** | Eso lo resuelve el despliegue —Kubernetes, Traefik, Consul— y lo resuelve mejor, porque ve todas las instancias. Un framework sólo ve el proceso en el que vive. |
| **Circuit Breaker**, **Bulkhead** | Pertenecen al cliente HTTP concreto o a la malla de servicios. Y mal ajustados son peligrosos: un breaker que abre antes de tiempo convierte una degradación parcial en una caída total. |
| **Comunicación entre microservicios** | No es una feature, es una decisión de arquitectura. Lo que Syrax sí puede aportar son las piezas: un cliente HTTP con timeouts y reintentos, y el outbox, que ya está. |

### La superficie todavía no tiene frontera

| Qué | Qué hay que resolver antes |
|---|---|
| **RabbitMQ**, **Kafka**, **SQS** | Cada uno es un SDK entero con su propio modelo: exchanges y bindings, particiones y offsets, visibility timeouts. Y **Kafka no es una cola, es un log**: forzarlo a las seis operaciones de `jobs::Driver` sería mentir sobre lo que hace. El día que necesitas Kafka de verdad, quieres Kafka, no la idea que Syrax se hizo de Kafka. |
| **OAuth2**, **OIDC** (como proveedor) | Son especificaciones, no features: discovery, JWKS, rotación de claves, PKCE, refresh, cuatro flujos y sus modos de fallo. La mitad finita —*validar* un token ajeno— **ya está hecha**, en `jwks.hpp`. Ser el proveedor, no. |
| **Sessions** | Otro modelo de identidad: cookie, almacén de sesión, CSRF, fijación, expiración deslizante. Para una API, el token ya cubre el caso; mezclar los dos modelos es donde aparecen los agujeros. |
| **Feature modules instalables** | Es un gestor de paquetes: resolución de versiones, dependencias entre módulos, puntos de extensión estables. Eso ya es CMake y FetchContent. |
| **Capa de idiomas (i18n)** | Para una API el mensaje traducido casi siempre lo pone el cliente, que es quien sabe el idioma del usuario. Y **el `code` estable de la capa de errores es justo lo que lo hace innecesario**: el servidor manda `saldo_insuficiente` y el cliente decide cómo se dice. |

### Habría dos formas de hacer lo mismo

| Qué | Qué hay que resolver antes |
|---|---|
| **Events + Event Bus** | Es lo que hacen las colas, con otro vocabulario. Dos formas de hacer lo mismo es peor que una: nadie sabe cuál usar y las dos se quedan a medias. |
| **Webhooks** | Un webhook es un POST con reintentos y una firma HMAC. Los reintentos son un job, y `hmacSha256` ya está en `auth.hpp`. Merece **una receta en el README**, no una feature. |
| **Lazy loading**, **identity map**, **cascadas** | Las relaciones de un nivel ya entraron: `with<U>()` trae los hijos en dos consultas fijas y `relations` las declara en el modelo. Lo que falta es lo que no tiene frontera: con lazy, el número de consultas pasa a depender de los datos —el N+1—, y anidar dos niveles ya es un planificador. Las cascadas, mientras tanto, las hace mejor un `ON DELETE CASCADE`, que también vale cuando el borrado viene de fuera de la app. |
| **Storage / S3**, **Mail** | Firmas, multipart, reintentos, TLS, adjuntos, rebotes. **Pero** el mail es el caso de libro de una interfaz —`Mailer` con `SmtpMailer` y `LogMailer`—, así que si algún día entra un puerto de ejemplo en el andamiaje, que sea éste y no un `IUserRepository`. |

---

## Qué más va en `bootstrap/`

El patrón —cada preocupación en su archivo, `create()` las llama en orden— dio
para todo lo que fue llegando. Hoy el andamiaje tiene siete:

| Archivo | Qué configura |
|---|---|
| `config.cpp` | Toda la configuración, tipada y validada. **Va primero** |
| `database.cpp` | La conexión a la base |
| `cache.cpp` | El Redis, si lo enciendes |
| `queue.cpp` | El driver de la cola y los jobs registrados |
| `schedule.cpp` | Las tareas periódicas |
| `middleware.cpp` | CORS, rate limit, cabeceras, ETag, `?fields`, y la autenticación que elijas |
| `errors.cpp` | Formato del error y traducción de excepciones |

Y dos que conviene **no** crear. `logging.cpp` no está en esa tabla a propósito:
`LOG_FORMAT`, `LOG_LEVEL` y `LOG_ACCESS` ya configuran el log, y `log::install()`
lo llama `App::run()` por su cuenta, así que el archivo no tendría ninguna
decisión que guardar —sólo releería variables que el framework ya lee—. Y
`routes.cpp` ya existe aparte y es donde debe
seguir. El día que `create()` tenga diez llamadas, el problema no es que falte
un archivo más: es que la aplicación necesita módulos, y eso es otra discusión.

## Inyección de dependencias

Syrax **no tiene contenedor**, y antes de añadirlo conviene mirar qué de lo que
se le pide a uno no está ya resuelto por el lenguaje.

| Ciclo de vida | En C++ | ¿Hace falta el contenedor? |
|---|---|---|
| **Singleton** | `db::client()`, `cache::client()`, `jobs::driver()` son estáticos de función o los gestiona Drogon. | No. Ya lo tienes, y sin registro global. |
| **Transient** | Construir el objeto: `UserService s{repo};` | No. Un contenedor para llamar a un constructor es ceremonia. |
| **Factory** | `std::function<T()>`. `jobs::Driver` ya recibe un `Provider` así. | No. |
| **Lazy** | Estático local de función, o `std::once_flag`. Inicializa en el primer uso y es seguro entre hilos por norma. | No. |
| **Scoped (por petición)** | `Request::get` / `Request::set` ya son la bolsa por petición: `request.get("auth.sub")` sale de ahí. | **Es el único que da problemas**, y no se arregla con un contenedor. |

El aviso que sí merece estar escrito: **con corrutinas, un `thread_local` no es
un scope de petición.** Un `co_await` puede reanudar la corrutina en otro hilo
del pool, y a partir de ahí el `thread_local` que leías es el de otra petición.
Eso no da un error: da datos del usuario equivocado, de vez en cuando. La forma
correcta en este diseño es pasar el contexto explícito —el `Request` o el
`Actor`— y ese camino ya está abierto.

**Recomendación:** no meter contenedor. En su lugar, dos entregables concretos:

1. Un apartado del README sobre el scope por petición y la trampa del
   `thread_local` con corrutinas. Esto previene un bug real.
2. `syrax make:port <Nombre>` para generar interfaz, implementación y doble de
   test **el día que la costura existe de verdad**, en vez de por si acaso.

Si aun así quieres el contenedor, el más pequeño que sirve es un mapa de
factorías por tipo con su ciclo de vida: unas 150 líneas. Lo que compras con
eso es poder cambiar la implementación en tiempo de ejecución. Lo que pagas es
que los errores de cableado dejan de ser de compilación y pasan a ser de
arranque —o peor, de la primera petición que toque esa rama—, y que cada
resolución pasa por un `virtual`, que en una corrutina impide elidir el marco.
Dicho de otro modo: el contenedor no añade desacoplamiento, añade *indirección
en tiempo de ejecución*. Sólo vale la pena cuando hace falta esa segunda parte.

---

## La capa de errores

Hoy el borde HTTP hace tres cosas con los fallos, y las tres están cerradas:

1. `Error{status, message}` se serializa siempre como `{"error":{"status","message"}}`.
2. Un 422 de validación añade `fields`, por un camino distinto.
3. Una excepción que se escapa se registra y sale como `500 internal server error`.

Eso cubre lo genérico y no deja sitio para lo propio: un `saldo_insuficiente`
que el cliente pueda ramificar sin parsear castellano, una API que necesite RFC
7807 o el sobre `{ok:false,...}` que ya usa su frontend, o un `UniqueViolation`
que merecía un 409 y sale como 500.

La propuesta son tres piezas pequeñas y un archivo nuevo, `errors.hpp`, más su
`src/bootstrap/errors.cpp` en el andamiaje —donde ya viven `database`, `cache` y
`queue`—. Todo con el comportamiento actual como valor por defecto: un proyecto
que no toque nada no cambia ni un byte de su respuesta.

### 1. `Error` gana un código estable

```cpp
struct Error {
    int         status;
    std::string message;
    std::string code;    // "saldo_insuficiente": para la maquina
    std::string detail;  // la version larga: para el humano
};
```

Ambos campos son opcionales y sólo aparecen en el JSON cuando no están vacíos,
así que los clientes que hoy leen `error.message` siguen igual. Se quedan como
`std::string` a propósito: meter un `Json::Value` aquí arrastraría jsoncpp a
`result.hpp`, que hoy no depende de nada. Lo estructurado se resuelve en el
formato (pieza 2), que es donde ya hay un `Json::Value` a mano.

Con eso, el catálogo de errores de un proyecto es sólo funciones que devuelven
`Error`, sin nada del framework:

```cpp
namespace errors {
inline syrax::Error saldoInsuficiente(double falta) {
    return {.status = 409, .message = "saldo insuficiente",
            .code = "saldo_insuficiente",
            .detail = std::format("faltan {:.2f} para cubrir el cargo", falta)};
}
}
```

### 2. El formato se puede sobrescribir, una sola vez

```cpp
syrax::onError([](const syrax::Error& e) -> drogon::HttpResponsePtr {
    Json::Value body;
    body["ok"]      = false;
    body["code"]    = e.code.empty() ? std::to_string(e.status) : e.code;
    body["message"] = e.message;
    return syrax::response(body, e.status);
});
```

Dos cosas que este gancho tiene que respetar para no empeorar lo que hay:

- **El 422 de validación pasa por aquí también.** Si no, un proyecto que
  sobrescribe el formato acaba con dos formas de error distintas en la misma
  API, que es peor que no poder sobrescribirlo. El `Error` que llega lleva los
  campos del fallo, y el gancho decide dónde ponerlos.
- **`applyResponseChain` se sigue aplicando después**, para que las cabeceras de
  seguridad y CORS no dependan de si alguien tocó el formato.

### 3. Las excepciones se pueden traducir

```cpp
syrax::onException([](const std::exception& e) -> std::optional<syrax::Error> {
    if (dynamic_cast<const drogon::orm::UniqueViolation*>(&e))
        return syrax::Conflict("el recurso ya existe");
    return std::nullopt;   // lo que no reconozcas sigue siendo un 500
});
```

`std::nullopt` es la clave: el gancho traduce lo que entiende y deja en paz lo
demás, en vez de obligar a cada proyecto a reimplementar el caso por defecto.

**El `what()` no viaja al cliente.** Hoy eso lo garantiza el framework; con un
gancho pasa a ser una decisión del proyecto, porque `e.what()` está ahí, a mano
y tentador. En su momento decía `no such table: users`, que le describe el
esquema a cualquiera que provoque el error. El log es donde sirve. Esto va en el
README junto al gancho, no escondido aquí.

### Por qué así y no con excepciones propias

La alternativa obvia es una jerarquía `SyraxException` que cada proyecto extiende
y el borde atrapa por tipo. Se descarta por lo mismo que `Error` no es una
excepción desde el principio: el camino normal de un fallo de negocio —un email
repetido, un saldo corto— es *esperado*, y pagar un `throw` con desenrollado de
pila en una corrutina por algo esperado es caro y además invisible en la firma de
la función. `Result<T>` dice en el tipo que esto puede fallar. Los ganchos de
arriba son para lo *inesperado*, que es justo donde una excepción sí encaja.
