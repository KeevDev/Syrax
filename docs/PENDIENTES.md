# Pendientes

Lo que está mal o a medias, con lo que se sabe de cada cosa. Separado del
[roadmap](ROADMAP.md), que es lo que falta **construir**; esto es lo que hay que
**arreglar**.

Ordenado por lo que más duele si no se toca.

---

## Bugs conocidos

### El abort de sqlite al terminar un test

`ctest -j8` aborta después de que un test **haya pasado**:

```
Connection is not ready - Sqlite3Connection.cc:173
```

Medido en **4 de 25** ejecuciones sobre el código ya commiteado. Bajó a **~1 de
30** haciendo que las fixtures de sqlite y el kit de tests dejen de soltar sus
clientes de Drogon —el mismo precedente que el cliente de Redis que
`jobs_test.cpp` deja vivo a propósito—. **No está arreglado.**

Qué pasa: se cierra un cliente de Drogon con callbacks todavía en vuelo, la
consulta se ejecuta sobre una conexión ya cerrada, la `BrokenConnection` viaja
por una corrutina que ya nadie espera y el proceso aborta.

Por qué importa más de lo que parece: en CI es **indistinguible de un fallo
real**, y una suite en la que no confías del todo deja de frenarte.

### El job `docker` del CI arrancaba una imagen vacía

**Arreglado**, pero merece quedar escrito porque estuvo así desde la 0.2.0 y
explica por qué ese job nunca se vio pasar.

El `Dockerfile` generado hacía `COPY . .` antes de compilar, así que cualquier
cambio recompilaba Drogon entero. Al partirlo en dos capas apareció el fallo de
verdad: `COPY` **conserva las fechas** de los archivos, el `main.cpp` real
llegaba con fecha anterior al `.o` que había dejado la capa de calentamiento,
ninja lo daba por actualizado y **la imagen salía con el binario de mentira
dentro** — arrancaba, no escribía una línea y salía con código 0.

Se arregló compilando en esa capa sólo el target de `drogon`, nunca el
proyecto: sin `.o` del proyecto, no hay `.o` que se quede rancio.

---

## Sin verificar

### El job `clang` del CI

Añadido en la 0.2.0 y **todavía no se ha visto pasar**. Localmente sí: la
librería, los tests y un proyecto generado compilan con Clang 22.

Mientras siga así, el README lo declara como intención y no como hecho — que es
lo correcto, pero lleva demasiado tiempo pendiente.

### El `static_assert` de `save()` sin clave primaria

El predicado `hasPrimaryKeyField<T>()` sí tiene tests (`query_test.cpp`), pero
que el `save()` prohibido **no compile** está verificado sólo a mano. Cubrirlo
de verdad necesita un `try_compile` aparte en CMake.

---

## Deuda

### La documentación se desincroniza más rápido que el código

En una sola revisión aparecieron cinco afirmaciones falsas en el README: el
número de tests (decía 137, eran 355), las líneas de cabecera (4.200 contra
9.600), el scheduler y los joins listados como no-objetivos cuando ya existían,
y una referencia a un apartado que **nunca existió**.

Todas corregidas, pero el patrón se repite y el README es lo que lee quien
llega. Lo que lo cerraría: un check en CI que compare contra el código lo que se
puede comparar —número de tests, líneas y número de cabeceras, comandos del
CLI—, y falle el build si no cuadra.

### Fugas de Drogon en la API pública

Medido en [SIN-DROGON.md](SIN-DROGON.md). Tres cosas concretas:

- **`Request::drogon()`** (`middleware.hpp:47`) expone el puntero de Drogon. El
  envoltorio existe; la fuga tiene nombre propio.
- **`drogon::app()` en 54 sitios**, con 19 métodos distintos y sin
  intermediario.
- **69 firmas públicas** que devuelven o reciben tipos de Drogon.

Cerrarlo **no compromete a salir de Drogon**: mejora la API por sí solo y deja
la puerta abierta.

### Dos estilos de `using` en el andamiaje

`UserRepository.cpp` (el de `syrax new`) califica `syrax::` en todo e importa
tres funciones de `db`. El que escribe `syrax make:api` hace `using namespace
syrax;` y lo deja sin calificar. Un proyecto acaba con dos repositorios lado a
lado escritos distinto. Unificar por el del generador.

### Un cliente HTTP por petición

`http.hpp:194` crea un `HttpClient` en cada `send()`, o sea un handshake por
llamada. **Es deliberado y está documentado ahí**: un `HttpClient` de Drogon
mantiene una sola conexión y trae el pipelining apagado, así que compartirlo
serializaría las llamadas concurrentes sobre un socket — y el temporizador del
timeout arranca al encolar, no al enviar, así que bajo carga caducarían
peticiones que nunca salieron.

Queda anotado porque **la salida correcta existe** y no está hecha: un pool de N
clientes, el día que el handshake sea de verdad el cuello de botella.

---

## Decisiones abiertas

### Tres filas de «lo que falta» que quizá no son «todavía no»

**API Gateway**, **Service Discovery** y **Load Balancing** están en la lista de
pendientes por decisión explícita —todo es objetivo—, pero el motivo que da el
roadmap no es que sean caras: es que **un proceso no puede ver las otras
instancias**. Eso no se resuelve con esfuerzo.

Si algún día se quiere que la lista sea del todo honesta, ésas son las tres que
irían aparte — no como renuncia, sino como «esto lo pone el despliegue».

### La imagen de Docker

Decidido que se hace: que alguien pueda crear y correr un proyecto Syrax **sin
instalar nada**, porque el obstáculo real no es `install.sh` sino tener un
compilador con C++23.

Hecho ya: la capa de dependencias cacheada, y `SYRAX_USE_SYSTEM_DEPS` para que
un entorno con Drogon ya compilado no lo vuelva a compilar.

Falta: publicar la imagen, añadir el servicio `app` al `docker-compose`
generado, y documentar el camino en el README al lado de `install.sh`.

Dos cosas que hay que resolver al hacerlo:

- **`syrax serve` dentro de un contenedor.** La vigilancia por inotify funciona
  sobre un bind mount en Linux; en macOS y Windows los eventos cruzan la VM de
  Docker Desktop y llegan tarde o no llegan. Hay salida —la tecla `r` y
  `--no-watch`—, pero no se puede vender recarga automática a quien no la va a
  tener.
- **El `build/` compartido entre host y contenedor.** Si alguien hace `docker
  compose up` y luego `syrax build` en su máquina, el `CMakeCache.txt` tiene
  rutas del contenedor y revienta con el error de generador que ya se ha visto
  en este proyecto. El build del contenedor tiene que vivir en un volumen con
  nombre, no en el bind mount.
