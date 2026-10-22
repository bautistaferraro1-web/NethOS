#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$ROOT/build/userland"
DIST="$WORK/distfiles"
SRC="$WORK/src"
JOBS="${JOBS:-$(nproc)}"

BASH_VER=5.3
BUSYBOX_VER=1.36.1

BASH_ARCHIVE="$DIST/bash-${BASH_VER}.tar.gz"
BUSYBOX_ARCHIVE="$DIST/busybox-${BUSYBOX_VER}.tar.bz2"

BASH_URL="https://ftp.gnu.org/gnu/bash/bash-${BASH_VER}.tar.gz"
BUSYBOX_URL="https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2"

# SHA-256 del archivo BusyBox 1.36.1 publicado oficialmente.
BUSYBOX_SHA256="b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314"
BASH_SHA256="0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba"

for cmd in curl tar sha256sum gcc make ld readelf file; do
    command -v "$cmd" >/dev/null 2>&1 || {
        echo "ERROR: falta la herramienta '$cmd'." >&2
        exit 1
    }
done

mkdir -p "$DIST" "$SRC"

fetch() {
    local url="$1"
    local dst="$2"

    if [[ ! -s "$dst" ]]; then
        echo "Descargando: $url"
        curl --fail --location --retry 3 --output "$dst.part" "$url"
        mv "$dst.part" "$dst"
    fi
}

verify_bash_archive() {
    if ! printf '%s  %s\n' "$BASH_SHA256" "$BASH_ARCHIVE" |
        sha256sum --check --status; then
        rm -f "$BASH_ARCHIVE"
        echo "Descargando nuevamente Bash: el archivo no coincide." >&2
        curl --fail --location --retry 3 \
            --output "$BASH_ARCHIVE.part" "$BASH_URL"
        printf '%s  %s\n' "$BASH_SHA256" "$BASH_ARCHIVE.part" |
            sha256sum --check
        mv "$BASH_ARCHIVE.part" "$BASH_ARCHIVE"
    fi

    printf '%s  %s\n' "$BASH_SHA256" "$BASH_ARCHIVE" |
        sha256sum --check
}

verify_busybox_archive() {
    if ! printf '%s  %s\n' "$BUSYBOX_SHA256" "$BUSYBOX_ARCHIVE" |
        sha256sum --check --status; then
        rm -f "$BUSYBOX_ARCHIVE"
        echo "Descargando nuevamente BusyBox: el archivo anterior no coincide." >&2
        curl --fail --location --retry 3 \
            --output "$BUSYBOX_ARCHIVE.part" "$BUSYBOX_URL"
        printf '%s  %s\n' "$BUSYBOX_SHA256" "$BUSYBOX_ARCHIVE.part" |
            sha256sum --check
        mv "$BUSYBOX_ARCHIVE.part" "$BUSYBOX_ARCHIVE"
    fi

    printf '%s  %s\n' "$BUSYBOX_SHA256" "$BUSYBOX_ARCHIVE" |
        sha256sum --check
}

fetch "$BASH_URL" "$BASH_ARCHIVE"
fetch "$BUSYBOX_URL" "$BUSYBOX_ARCHIVE"
verify_bash_archive
verify_busybox_archive

# Usar directorios de construcción limpios.
rm -rf "$SRC/bash-${BASH_VER}" "$SRC/busybox-${BUSYBOX_VER}"
tar -xzf "$BASH_ARCHIVE" -C "$SRC"
tar -xjf "$BUSYBOX_ARCHIVE" -C "$SRC"

BASH_SRC="$SRC/bash-${BASH_VER}"
BUSYBOX_SRC="$SRC/busybox-${BUSYBOX_VER}"

echo
echo "===== CONSTRUIR BASH CON READLINE ====="
(
    cd "$BASH_SRC"
    CFLAGS="-O2 -no-pie -fno-pie" \
    LDFLAGS="-static -no-pie" \
        ./configure \
            --enable-static-link \
            --without-bash-malloc \
            --disable-nls \
            --without-curses

    make -j"$JOBS"
)

if ! grep -q '^#define READLINE 1' "$BASH_SRC/config.h"; then
    echo "ERROR: Bash se compiló sin la configuración READLINE esperada." >&2
    exit 1
fi

echo
echo "===== CONSTRUIR BUSYBOX ESTÁTICO ====="
(
    cd "$BUSYBOX_SRC"
    make defconfig

    # BusyBox 1.36.1: TC usa definiciones CBQ eliminadas de headers Linux modernos.
    sed -i -e 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' .config

    sed -i \
        -e 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' \
        -e 's/^CONFIG_STATIC=.*/CONFIG_STATIC=y/' \
        .config

    if ! grep -q '^CONFIG_STATIC=y$' .config; then
        echo "ERROR: no se pudo habilitar CONFIG_STATIC." >&2
        exit 1
    fi

    # oldconfig responde con los valores predeterminados.
    set +o pipefail
    yes "" | make oldconfig
    set -o pipefail

    grep -q '^CONFIG_STATIC=y$' .config || {
        echo "ERROR: la configuración final no es estática." >&2
        exit 1
    }

    if grep -q '^CONFIG_TC=y$' .config; then
        echo "ERROR: el applet TC sigue habilitado." >&2
        exit 1
    fi

    make -j"$JOBS"
)

BASH_BIN="$BASH_SRC/bash"
BUSYBOX_BIN="$BUSYBOX_SRC/busybox"

echo
echo "===== VALIDAR EJECUTABLES ====="
for bin in "$BASH_BIN" "$BUSYBOX_BIN"; do
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: no se generó $bin" >&2
        exit 1
    fi

    if readelf -l "$bin" | grep -q 'INTERP'; then
        echo "ERROR: $bin requiere un intérprete dinámico." >&2
        exit 1
    fi

    file "$bin"
done

"$BUSYBOX_BIN" --list > "$WORK/busybox-applets.txt"

echo
echo "===== COMPROBAR ENLACES DE INITRD ====="
missing=()

while IFS= read -r applet; do
    [[ -n "$applet" ]] || continue

    if ! grep -Fxq -- "$applet" "$WORK/busybox-applets.txt"; then
        missing+=("$applet")
    fi
done < <(
    find "$ROOT/initrd/bin" -maxdepth 1 \
        -type l -lname busybox -printf '%f\n' | sort -u
)

if (( ${#missing[@]} > 0 )); then
    printf 'ERROR: faltan applets de BusyBox:\n' >&2
    printf '  %s\n' "${missing[@]}" >&2
    echo "No se reemplazaron los binarios del initramfs." >&2
    exit 1
fi

# Instalar solamente cuando ambas compilaciones pasaron las validaciones.
install -m 755 "$BASH_BIN" "$ROOT/initrd/bin/bash.new"
install -m 755 "$BUSYBOX_BIN" "$ROOT/initrd/bin/busybox.new"
mv -f "$ROOT/initrd/bin/bash.new" "$ROOT/initrd/bin/bash"
mv -f "$ROOT/initrd/bin/busybox.new" "$ROOT/initrd/bin/busybox"

echo
echo "===== VERSIONES INSTALADAS ====="
"$ROOT/initrd/bin/bash" --version | head -n 1
"$ROOT/initrd/bin/busybox" 2>&1 | head -n 1 || true

echo
echo "OK: Bash y BusyBox reconstruidos e instalados en initrd/bin."
echo "Ahora podés ejecutar: cd \"$ROOT\" && make run"
