# dkv

API construida con [Syrax](https://github.com/KeevDev/Syrax). Motor: **PostgreSQL**.

## Arrancar

```bash
docker compose up -d
syrax migrate
syrax db:seed     # opcional: datos de ejemplo
syrax serve
```

```bash
curl localhost:8080/users
curl -X POST localhost:8080/users -H 'Content-Type: application/json' \
     -d '{"name":"Kevin","email":"kev@example.com","age":30}'
```

## Estructura

```
config/app.json           ajustes del servidor (versionado)
.env                      credenciales (NO versionado)

database/
├── migrations.cpp        que migraciones existen y en que orden
├── migrations/           las migraciones    -> syrax migrate
├── seeders/              datos demo         -> syrax db:seed
└── factories/            objetos de mentira para tests

src/
├── main.cpp              arranque y conexion a la BD
├── routes/               el mapa de la API
│   ├── routes.cpp        engancha las versiones
│   └── v1.cpp            rutas de /api/v1
├── http/                 TODO lo atado al transporte
│   ├── controllers/User/ recibe, delega, responde
│   ├── requests/User/    lo que entra
│   └── resources/User/   lo que sale
├── services/User/        logica de negocio
├── repositories/User/    SQL. lo unico que sabe de la BD
└── models/User/          la forma de la tabla
```

**Por que existe `http/`:** marca un limite real. Si manana expones la misma
logica por gRPC, esa carpeta se tira entera y `services/`, `repositories/` y
`models/` siguen sirviendo sin tocarse.

Cada capa se subdivide por recurso (`User/`, `Order/`) para que con veinte
entidades ninguna carpeta sea un basurero plano.

## Versionar la API

`src/routes/v1.cpp` monta todo bajo `/api/v1`. Para una v2: copia ese archivo,
cambia `kPrefix`, y registralo en `routes.cpp`. Las dos versiones conviven y
pueden apuntar a controladores distintos.

**Por que models/ y resources/ estan separados:** `User` tiene `passwordHash`
y `UserResource` no. Un campo privado no puede filtrarse por accidente porque
el tipo que se serializa simplemente no lo tiene.

## Agregar un recurso

1. Migracion en `database/migrations/` y su linea en `database/migrations.cpp`
2. `models/Product/Product.hpp` — un struct plano con los campos de la tabla
3. `repositories/Product/ProductRepository.*` — el SQL
4. `services/Product/ProductService.*` — las reglas
5. `http/requests/Product/` y `http/resources/Product/`
6. `http/controllers/Product/` y registralo en `src/routes/v1.cpp`

No hay que tocar el `CMakeLists.txt`.

## Sobre el mapeo

Syrax convierte filas a structs reflejando los nombres de campo en tiempo de
compilacion: el campo `email` se llena con la columna `email`. No hay que
escribir ese mapeo ni generar modelos de 500 lineas.
