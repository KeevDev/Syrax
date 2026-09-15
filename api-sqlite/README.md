# api-sqlite

API construida con [Syrax](https://github.com/KeevDev/Syrax). Motor: **SQLite**.

## Arrancar

```bash
cp .env.example .env
sqlite3 app.db < migrations/001_create_users.sql
syrax serve
```

```bash
curl localhost:8080/users
curl -X POST localhost:8080/users -H 'Content-Type: application/json' \
     -d '{"name":"Kevin","email":"kev@example.com","age":30}'
```

## Estructura

```
src/
├── main.cpp              arranque y conexion a la BD
├── routes.*              donde se arma la API
├── controllers/          HTTP: recibe, delega, responde
├── services/             logica de negocio
├── repositories/         SQL. lo unico que sabe de la BD
├── models/               la forma de la tabla
├── requests/             lo que entra
└── resources/            lo que sale
```

**Por que models/ y resources/ estan separados:** `User` tiene `passwordHash`
y `UserResource` no. Un campo privado no puede filtrarse por accidente porque
el tipo que se serializa simplemente no lo tiene.

## Agregar un recurso

1. Migracion en `migrations/`
2. `models/Product.hpp` — un struct plano con los campos de la tabla
3. `repositories/ProductRepository.*` — el SQL
4. `services/ProductService.*` — las reglas
5. `resources/ProductResource.*` y `requests/ProductRequests.hpp`
6. `controllers/ProductController.*` y registralo en `src/routes.cpp`

No hay que tocar el `CMakeLists.txt`.

## Sobre el mapeo

Syrax convierte filas a structs reflejando los nombres de campo en tiempo de
compilacion: el campo `email` se llena con la columna `email`. No hay que
escribir ese mapeo ni generar modelos de 500 lineas.
