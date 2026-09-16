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
| **Rate limiting** | `middleware.hpp` → `rateLimit()` | El contador vive en el proceso. La versión compartida está en el nivel 2. |
| **CORS configurable** | `middleware.hpp` → `CorsOptions` | Ya es configurable: `origins`, `methods`, `headers`, `credentials`, `maxAge`, y desde el nivel 1 viene cableado en `bootstrap/middleware.cpp`. |
| **API Keys** | `middleware.hpp` → `requireApiKey()` | Con cabecera configurable. |
| **JWT** | `auth.hpp` → `sign`, `verify`, `bearer` | Y `hashPassword`/`verifyPassword` con PBKDF2, que suele faltar. |
| **Guards** | `middleware.hpp` + `policy.hpp` | Un guard es un middleware que devuelve `Error`. Es el mismo concepto con otro nombre. |
| **Policies / ABAC** | `policy.hpp` | Una policy *es* una función sobre actor + recurso: eso ya es ABAC, sin el vocabulario. |
| **RBAC** | `policy.hpp` → `requireRole(actor, "admin", "editor")` | Roles simples. Una tabla de permisos editable en runtime es aplicación, no framework. |
| **DTOs** | `resources/` y `requests/` del andamiaje | Son DTOs de entrada y de salida, con el nombre que usa el README. |
| **Cache abstraction** | `cache.hpp` → `get/put/forget/has/remember` | La abstracción existe; el único driver es Redis. |
| **Connection pooling configurable** | `db::Connection::connections`, env `DB_POOL` | |
| **Multi-database** | `db::client("nombre")` + `queryOn` / `executeOn` / `transactionOn` | **Ya funciona hoy.** Lo que falta es azúcar en bootstrap y un apartado en el README, no código. |
| **Manejo de response JSON** | `Result<T>` + `makeOk` / `makeError` | El handler devuelve el valor o el `Error`; el borde lo traduce. |
| **Outbox pattern** | `jobs::dispatch(job, tx)` | Encolar dentro de la misma transacción que escribe la fila es el 80% del outbox, y es la parte que importa. |
| **Documentación OpenAPI** | `openapi.hpp` | Generada desde los tipos, no escrita a mano. |
| **Interfaces** | — | Cuando hay dos implementaciones reales, sí: `jobs::Driver` lo es. Con una sola es un vtable y un archivo de más. Ver *Inyección de dependencias*, abajo. |

---

## Nivel 1 — hecho

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

Dos cosas que cambiaron de plan por el camino:

- **No hay `bootstrap/logging.cpp`.** Las tres variables de entorno ya configuran
  el log, y un archivo que sólo las relee es ruido en un directorio donde cada
  archivo debería justificar su sitio.
- **La vista en vivo no la pinta el CLI.** Parsear el stdout del hijo era la idea
  original; el framework ya sabe si está delante de una terminal, así que la
  línea en color la escribe él. Menos piezas y el mismo resultado.

## Nivel 2 — valor alto, superficie finita

| Qué | Por qué | Coste |
|---|---|---|
| **Kit de tests** `syrax::testing` | Sigue siendo el hueco real: los tests generados no tocan servicio ni repositorio. Un fixture sqlite más un cliente HTTP en proceso permite un test que va ruta → controller → service → repositorio → SQL de verdad. Es lo único del nivel 1 que quedó fuera, por tamaño. | Medio |
| **Puerto libre en los tests del propio Syrax** | `tests/test_server.cpp` usa un puerto fijo, así que `ctest -j8` hace colisionar los procesos que levantan servidor. En serie pasan los 209. Pedir un puerto libre al arrancar lo arregla. | Bajo |
| **Configuración tipada** | Hoy todo es `env("DB_POOL", "4")`: strings y un fallo tipográfico que no se nota hasta producción. Con la reflexión de Glaze que ya se usa, un `struct Config` se puede llenar desde el entorno y **validar al arrancar**, con el nombre del campo en el error. Encaja con la casa. | Medio |
| **Scheduler** | Era no-objetivo cuando no había colas. Ahora reusa `jobs::dispatch` y el delta es mínimo. Finito si se rechazan las expresiones cron y se ofrece `every(5min)` / `dailyAt("03:00")`. | Bajo |
| **Rate limit con Redis** | El actual es un mapa en memoria: con dos instancias el límite se duplica. El cliente Redis ya está. | Bajo |
| **`Idempotency-Key`** | El reintento del cliente crea el pedido dos veces. Casi ningún framework lo trae y en una API que cobra es obligatorio. Finito: guardar la respuesta N horas. | Bajo |
| **`/health` de verdad** | El del andamiaje devuelve 200 fijo, así que el healthcheck de Docker miente cuando la base está caída. Que pregunte a cada cliente registrado. | Bajo |
| **Paginación estándar** | `limit`/`offset` están, pero cada API reinventa el sobre `{data, total, page}`. Que `Query<T>::paginate(page, per)` lo devuelva y OpenAPI lo documente. | Bajo |
| **Serialización parcial** (`?fields=id,nombre`) | El cliente móvil no quiere 30 campos. Con `glz::reflect` los nombres ya están en tiempo de compilación, así que filtrar es barato y no hace falta un lenguaje de query. | Medio |
| **Validar tokens de terceros** (`auth::jwks(url)`) | La parte finita y útil de OAuth2/OIDC: verificar la firma de un token de Auth0, Keycloak o Cognito contra su JWKS, con caché de claves. *Ser* el proveedor no entra (ver abajo). | Medio |
| **Scopes** | `requireScope(actor, "pedidos:escribir")` junto a `requireRole`. Veinte líneas, y es lo que esperan los tokens de terceros del punto anterior. | Bajo |
| **Soft deletes y timestamps** | `deleted_at` filtrado por defecto, `created_at`/`updated_at` automáticos. La conveniencia que todo el mundo reimplementa mal. | Medio |
| **Subida de archivos con reglas** | `request.file("avatar")` con límites de tamaño y mime en el mismo lenguaje que `validation`. El guardado no: eso es storage, y storage no es finito. | Medio |
| **ETag / 304** | Para GET de recursos que cambian poco. Una línea en la cadena de respuesta. | Bajo |
| **Auditoría** | Tabla append-only con quién, qué y cuándo, alimentada desde el `Actor` y el request-id. Se apoya entera en la capa de trazabilidad. | Medio |
| **Cliente HTTP** | Toda API llama a otra API. Drogon ya trae el cliente; lo que falta es lo que siempre se escribe a mano mal: timeout por defecto, reintento con espera creciente sólo en métodos idempotentes, y **propagar el `X-Request-Id`** para que la traza cruce el salto. Se planta ahí: sin breaker, sin descubrimiento. | Medio |
| **CLI: `cache:clear`, `db`, `redis`** | Consolas y limpieza. Son envoltorios del cliente que ya está configurado en el proyecto: el valor es no tener que recordar el puerto ni la contraseña del `.env`. | Bajo |
| **Métricas** | Frontera: cuatro contadores en `/metrics` sí es finito; el día que alguien pida histogramas con labels, no. Después del nivel 1. | Medio |
| **`syrax new` con asistente** | Preguntar base, cache, auth y docs en vez de sólo el motor. **Con cuidado:** cada eje multiplica las combinaciones que hay que compilar en el CI. Entra sólo si cada pregunta cambia archivos de verdad, y con pocos ejes. | Medio |
| **Multi-tenancy por fila** | `tenant_id` con un scope global en `Query<T>`. Sólo el modelo de fila: el de esquema y el de base por tenant son decisiones que no se pueden desandar, y un framework no debería elegirlas por ti. | Medio |

## Nivel 3 — se quedan fuera, y por qué

Agrupados por la razón, que es más útil que una lista plana.

### Es infraestructura, no framework

| Qué | Por qué no |
|---|---|
| **API Gateway**, **Service Discovery** | Eso lo resuelve el despliegue —Kubernetes, Traefik, Consul— y lo resuelve mejor, porque ve todas las instancias. Un framework sólo ve el proceso en el que vive. |
| **Circuit Breaker**, **Bulkhead** | Pertenecen al cliente HTTP concreto o a la malla de servicios. Y mal ajustados son peligrosos: un breaker que abre antes de tiempo convierte una degradación parcial en una caída total. |
| **Comunicación entre microservicios** | No es una feature, es una decisión de arquitectura. Lo que Syrax sí puede aportar son las piezas: un cliente HTTP con timeouts y reintentos, y el outbox, que ya está. |

### La superficie no es finita

| Qué | Por qué no |
|---|---|
| **RabbitMQ**, **Kafka**, **SQS** | Cada uno es un SDK entero con su propio modelo: exchanges y bindings, particiones y offsets, visibility timeouts. Y **Kafka no es una cola, es un log**: forzarlo a las seis operaciones de `jobs::Driver` sería mentir sobre lo que hace. El día que necesitas Kafka de verdad, quieres Kafka, no la idea que Syrax se hizo de Kafka. |
| **OAuth2**, **OIDC** (como proveedor) | Son especificaciones, no features: discovery, JWKS, rotación de claves, PKCE, refresh, cuatro flujos y sus modos de fallo. La mitad finita —*validar* un token ajeno— está en el nivel 2. Ser el proveedor, no. |
| **Sessions** | Otro modelo de identidad: cookie, almacén de sesión, CSRF, fijación, expiración deslizante. Para una API, el token ya cubre el caso; mezclar los dos modelos es donde aparecen los agujeros. |
| **Feature modules instalables** | Es un gestor de paquetes: resolución de versiones, dependencias entre módulos, puntos de extensión estables. Eso ya es CMake y FetchContent. |
| **Capa de idiomas (i18n)** | Para una API el mensaje traducido casi siempre lo pone el cliente, que es quien sabe el idioma del usuario. Y **el `code` estable de la capa de errores es justo lo que lo hace innecesario**: el servidor manda `saldo_insuficiente` y el cliente decide cómo se dice. |

### Se solapa con algo que ya existe

| Qué | Por qué no |
|---|---|
| **Events + Event Bus** | Es lo que hacen las colas, con otro vocabulario. Dos formas de hacer lo mismo es peor que una: nadie sabe cuál usar y las dos se quedan a medias. |
| **Webhooks** | Un webhook es un POST con reintentos y una firma HMAC. Los reintentos son un job, y `hmacSha256` ya está en `auth.hpp`. Merece **una receta en el README**, no una feature. |
| **Relaciones en `Query<T>`** | Ahí es exactamente donde el query builder deja de ser finito. Ya está razonado en el README. |
| **Storage / S3**, **Mail** | Firmas, multipart, reintentos, TLS, adjuntos, rebotes. **Pero** el mail es el caso de libro de una interfaz —`Mailer` con `SmtpMailer` y `LogMailer`—, así que si algún día entra un puerto de ejemplo en el andamiaje, que sea éste y no un `IUserRepository`. |

---

## Qué más va en `bootstrap/`

Hoy el andamiaje tiene `app`, `database`, `cache` y `queue`. El patrón funciona
—cada preocupación en su archivo, `create()` las llama en orden— y varios puntos
de arriba aterrizan justo ahí:

| Archivo | Qué configura | Viene de |
|---|---|---|
| `middleware.cpp` | CORS, rate limit, cabeceras de seguridad, api key | Nivel 1 |
| `errors.cpp` | Formato del error y traducción de excepciones | Nivel 1 |
| `logging.cpp` | Destino, formato y nivel del log; request-id | Nivel 1 |
| `schedule.cpp` | Tareas periódicas | Nivel 2 |

Y uno que conviene **no** crear: `routes.cpp` ya existe aparte y es donde debe
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
