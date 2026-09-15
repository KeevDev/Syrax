# Build y runtime separados: la imagen final no carga compilador ni fuentes.
# Ambas etapas usan la misma base para que las versiones de las librerias
# compartidas coincidan.
#
# Si `apt-get install` falla en la etapa runtime por un nombre de paquete,
# comprueba el soname en tu version de Debian:
#     docker run --rm debian:trixie-slim sh -c "apt-get update && apt-cache search jsoncpp"
# El sufijo de libjsoncpp cambia entre releases (25 en bookworm, 26 en trixie).
FROM debian:trixie AS build

RUN apt-get update && apt-get install -y --no-install-recommends         g++ cmake ninja-build git ca-certificates pkg-config         libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev         libpq-dev libsqlite3-dev libc-ares-dev libbrotli-dev     && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release     && cmake --build build

FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends         libjsoncpp26 libuuid1 zlib1g libssl3t64 libpq5 libsqlite3-0         libc-ares2 libbrotli1 ca-certificates     && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/dkv /app/dkv
COPY --from=build /src/config /app/config
COPY --from=build /src/public /app/public

EXPOSE 8080
CMD ["/app/dkv"]
