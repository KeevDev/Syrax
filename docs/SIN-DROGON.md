# Qué habría que hacer para no depender de Drogon

Este documento no propone hacerlo. Mide el tamaño real de la dependencia, para
que el día que la decisión se plantee se tome con números y no con ganas.

Todo lo de aquí sale de `grep` sobre `include/syrax/`, no de memoria.

---

## El inventario

Símbolos de Drogon usados en las cabeceras, por frecuencia:

| Veces | Qué |
|---|---|
| 122 | `drogon::Task` |
| 54 | `drogon::app()` |
| 69 | tipos suyos en firmas públicas (`HttpRequestPtr`, `HttpResponsePtr`, `DbClientPtr`) |
| 27 | `drogon::orm::DbClientPtr` |
| 23 + 19 | `HttpRequestPtr` / `HttpResponsePtr` |
| 9 | `WebSocketConnectionPtr` |
| 8 | `drogon::async_run` |
| 5 | `AdviceCallback` / `AdviceChainCallback` |
| 3 | `nosql::RedisClientPtr` |

Repartidos así:

```
100  app.hpp          ← aquí está el 40%
 52  jobs.hpp
 29  query.hpp
 27  db.hpp
 24  http.hpp
 14  ws.hpp
 11  schedule.hpp
  9  middleware.hpp · errors.hpp · cache.hpp
  8  audit.hpp
  ≤5 el resto
```

Y lo que Drogon hace por nosotros, en líneas de código suyas:

| | Líneas |
|---|---|
| `lib/src` — HTTP, ruteo, advices | 22.952 |
| `trantor` — event loop, sockets, TLS | 22.073 |
| `orm_lib/src` — pool, prepared statements, transacciones | 10.644 |
| `nosql_lib` — Redis | 3.869 |
| **Total** | **~59.500** |

Syrax son ~10.000 líneas de cabeceras. **La dependencia es seis veces el
proyecto**, y es código probado en producción por mucha gente durante años.

---

## Capa por capa

### 1. El ORM — lo más fácil, y es contraintuitivo

Todo el mundo asume que el ORM es lo difícil. Aquí es lo más sencillo, porque
`db.hpp` y `query.hpp` **ya son la abstracción**: ningún servicio ni repositorio
del usuario habla con Drogon, hablan con `db::query` y `Query<T>`.

De los 10.644 líneas de `orm_lib`, Syrax usa **doce cosas**:

```
execSqlCoro          27      newTransactionCoro   1
execSqlSync          13      newPgClient          1
.as<T>()              8      newSqlite3Client     1
affectedRows          7      newMysqlClient       1
SqlBinder             4      UniqueViolation      1
SqlAwaiter            1      DrogonDbException    1
```

Para sustituirlo hace falta: un pool de conexiones, parámetros enlazados
(`libpq`, `libmysqlclient`, `sqlite3`), transacciones, y un awaitable por
consulta. Es trabajo de verdad —dos o tres mil líneas— pero **es finito y está
acotado**, y el resto del framework no se entera.

Lo que **no** hay que reescribir: el mapeo fila → struct (es Glaze), el query
builder, el schema builder, las migraciones. Todo eso ya es nuestro.

### 2. El event loop — lo que de verdad ata

`trantor` es el corazón: epoll, timers, buffers, TLS, el pool de hilos.

Las corrutinas de Syrax reanudan **en su loop**. Cada `co_await` de
`detail::run`, de Redis, de `sendRequestCoro` o de `sleepCoro` acaba ahí.

Sustituto realista: **Boost.Asio**. No hay tercera opción seria en C++.
Y entonces `syrax::Task` dejaría de ser un alias de dos líneas para pasar a ser
código con `promise_type` y awaiters propios, o `asio::awaitable`.

### 3. El HTTP y el ruteo — el más caro

Parser de HTTP/1.1, keep-alive, chunked, multipart, compresión, TLS, y el
ruteo con sus advices. Nada de eso tiene atajo y nada de eso es negociable
para un framework de APIs.

`registerHandler` es lo que hace funcionar la deducción por firma —la idea
central de Syrax—, así que esa pieza se reescribe entera.

### 4. Redis

`nosql_lib` son 3.869 líneas, y Syrax usa poco: comandos y scripts Lua. Hay
clientes de Redis en C++ (`redis-plus-plus`, `hiredis`), así que aquí sí hay
recambio de estantería. Pero tendría que hablar con el event loop nuevo.

### 5. WebSockets

Handshake, framing, ping/pong. `ws.hpp` tiene 14 referencias y `Room` ya es
nuestra. Es la pieza más pequeña.

---

## Las costuras que ya existen

No están puestas para esto, pero ayudan:

| Costura | Qué absorbe |
|---|---|
| `using Task = drogon::Task<T>` (`app.hpp:41`) | 122 usos detrás de **una línea** |
| `namespace db` (`db.hpp`) | El ORM entero detrás de 5 funciones |
| `Query<T>` | El SQL. No conoce a Drogon salvo por `DbClientPtr` |
| `syrax::Request` (`middleware.hpp:22`) | Envuelve `HttpRequestPtr`… |

## Y las que faltan

**`Request::drogon()` es una fuga con nombre.** El envoltorio existe, pero
expone el puntero de Drogon, y `uploads.hpp` y los middlewares lo usan. Mientras
esté ahí, el tipo de Drogon está en la API pública.

**`drogon::app()` se llama en 54 sitios, sin intermediario.** Diecinueve
métodos distintos: `registerHandler`, `addListener`, `getLoop`, `run`, `quit`,
los cuatro tipos de advice, los clientes de base y Redis, los manejadores de
señal. Esto es el lock-in real, y está concentrado en `app.hpp` y `jobs.hpp`.

**69 firmas públicas devuelven o reciben tipos de Drogon.** Cada una es un
punto donde el usuario del framework toca Drogon sin saberlo.

---

## Si algún día se hace, en este orden

1. **Cerrar las fugas primero, sin cambiar nada por debajo.** Un
   `syrax::detail::engine()` que envuelva los 19 métodos de `app()`, quitar
   `Request::drogon()` de la API pública, y que ninguna firma pública nombre a
   Drogon. Esto se puede hacer **hoy**, tiene valor por sí mismo —la API queda
   más limpia— y no compromete a nada.

2. **El ORM, que es independiente del resto.** Sustituir `db::` por un pool y
   unos awaitables propios sobre `libpq`/`sqlite3`. Si sale mal, se revierte sin
   tocar el HTTP.

3. **El event loop, sobre Asio.** Aquí `Task` deja de ser un alias.

4. **El HTTP y el ruteo.** Lo más caro, y lo último porque depende de 3.

5. **Redis y WebSockets.** Encima de lo anterior.

---

## La pregunta honesta

Antes de empezar por el 2, conviene tener respuesta a esto:

- **¿Qué se gana?** Si la respuesta es "no depender de nadie", eso no es una
  ganancia para quien usa el framework: para él, Drogon es un detalle que ya no
  ve. Si es "Drogon nos impide hacer X", entonces X es la razón y hay que
  escribirla aquí.

- **¿Quién lo mantiene?** 59.500 líneas de HTTP, TLS, corrutinas y drivers de
  base es un proyecto a tiempo completo, no un fin de semana. Drogon lleva años
  y mucha gente encontrando sus bugs.

- **¿Y el paso 1 no basta?** Cerrar las fugas da casi todo el beneficio
  práctico —una API propia, sin Drogon asomando— por una fracción del coste. Y
  deja la puerta abierta para el resto sin cerrarla nunca.

El paso 1 es útil hoy pase lo que pase. Del 2 en adelante, sólo si hay una X.
