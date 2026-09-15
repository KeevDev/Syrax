#!/bin/sh
# Instalador de syrax.
#
# Solo compila e instala el CLI, que no depende de Drogon ni de Glaze: son
# unos segundos y cero descargas. Las librerias las baja cada proyecto por
# FetchContent cuando corres `syrax build`.
#
#   ./install.sh                      instala en ~/.local/bin
#   ./install.sh --prefix /usr/local  instala en otro sitio
#   ./install.sh --add-to-path        ademas agrega el bin a tu shell rc
#   ./install.sh --uninstall          lo quita
set -eu

REPO_URL="https://github.com/KeevDev/Syrax.git"
PREFIX="${SYRAX_PREFIX:-$HOME/.local}"
ADD_TO_PATH=0
UNINSTALL=0
TMPDIR_CLONE=""

say()  { printf '%s\n' "$*"; }
step() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33maviso:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

cleanup() {
    [ -n "$TMPDIR_CLONE" ] && rm -rf "$TMPDIR_CLONE"
    return 0
}
trap cleanup EXIT

usage() {
    cat <<EOF
instalador de syrax

uso: ./install.sh [opciones]

  --prefix <dir>    directorio de instalacion (default: ~/.local)
  --add-to-path     agrega el bin a tu ~/.zshrc o ~/.bashrc
  --uninstall       desinstala
  -h, --help        esta ayuda
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)      [ $# -ge 2 ] || die "--prefix necesita un directorio"; PREFIX="$2"; shift 2 ;;
        --add-to-path) ADD_TO_PATH=1; shift ;;
        --uninstall)   UNINSTALL=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        *)             die "opcion desconocida: $1  (usa --help)" ;;
    esac
done

BIN_DIR="$PREFIX/bin"
TARGET="$BIN_DIR/syrax"

# ------------------------------------------------------------ desinstalacion
if [ "$UNINSTALL" -eq 1 ]; then
    if [ -f "$TARGET" ]; then
        rm -f "$TARGET"
        say "syrax desinstalado de $TARGET"
    else
        say "no habia nada instalado en $TARGET"
    fi
    exit 0
fi

# ------------------------------------------------------------- prerrequisitos
step "verificando prerrequisitos"
command -v cmake >/dev/null 2>&1 || die "falta cmake"
command -v git   >/dev/null 2>&1 || die "falta git"
command -v c++   >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 \
    || command -v clang++ >/dev/null 2>&1 || die "no encontre un compilador de C++"
say "    cmake $(cmake --version | head -1 | awk '{print $3}')"

# ------------------------------------------------------- localizar el codigo
if [ -f "CMakeLists.txt" ] && grep -q 'project(syrax' CMakeLists.txt 2>/dev/null; then
    SRC="$(pwd)"
    step "usando el repositorio local"
else
    step "clonando $REPO_URL"
    TMPDIR_CLONE="$(mktemp -d)"
    git clone --depth 1 "$REPO_URL" "$TMPDIR_CLONE/syrax" >/dev/null 2>&1 \
        || die "no pude clonar $REPO_URL"
    SRC="$TMPDIR_CLONE/syrax"
fi

# --------------------------------------------------------------- compilacion
step "compilando el CLI"
BUILD="$(mktemp -d)"
GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"

# shellcheck disable=SC2086
cmake -S "$SRC" -B "$BUILD" $GEN \
      -DCMAKE_BUILD_TYPE=Release \
      -DSYRAX_BUILD_LIB=OFF \
      -DSYRAX_BUILD_EXAMPLES=OFF \
      -DSYRAX_BUILD_CLI=ON >/dev/null 2>&1 || die "fallo la configuracion de cmake"

cmake --build "$BUILD" >/dev/null 2>&1 || die "fallo la compilacion"

[ -f "$BUILD/cli/syrax" ] || die "no se genero el binario"

# --------------------------------------------------------------- instalacion
step "instalando en $TARGET"
mkdir -p "$BIN_DIR"
if command -v install >/dev/null 2>&1; then
    install -m 755 "$BUILD/cli/syrax" "$TARGET"
else
    cp "$BUILD/cli/syrax" "$TARGET"
    chmod 755 "$TARGET"
fi
rm -rf "$BUILD"

# ----------------------------------------------------------------- el PATH
case ":${PATH}:" in
    *":${BIN_DIR}:"*) ON_PATH=1 ;;
    *)                ON_PATH=0 ;;
esac

if [ "$ON_PATH" -eq 1 ]; then
    say ""
    say "listo. syrax $("$TARGET" version | awk '{print $2}') instalado."
elif [ "$ADD_TO_PATH" -eq 1 ]; then
    case "${SHELL:-}" in
        */zsh)  RC="$HOME/.zshrc" ;;
        */bash) RC="$HOME/.bashrc" ;;
        *)      RC="" ;;
    esac

    if [ -n "$RC" ]; then
        printf '\n# agregado por el instalador de syrax\nexport PATH="%s:$PATH"\n' "$BIN_DIR" >> "$RC"
        say ""
        say "listo. agregue $BIN_DIR a $RC"
        say "abre una terminal nueva, o corre:  export PATH=\"$BIN_DIR:\$PATH\""
    else
        warn "no reconozco tu shell (${SHELL:-desconocido}); agrega esto a mano:"
        say "  export PATH=\"$BIN_DIR:\$PATH\""
    fi
else
    say ""
    say "listo, pero $BIN_DIR no esta en tu PATH."
    say "agrega esta linea a tu shell rc:"
    say ""
    say "  export PATH=\"$BIN_DIR:\$PATH\""
    say ""
    say "o vuelve a correr el instalador con --add-to-path"
fi

say ""
say "empieza con:  syrax new mi-api && cd mi-api && syrax serve"
