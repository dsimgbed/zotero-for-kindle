#!/bin/sh
# Cross-builds the app for the Kindle (kindlehf) inside the zk-kindlehf container.
#   tools/build-kindle.sh [extra meson -D options...]
# crengine and its dependencies are built first (tools/build-crengine.sh kindle) if missing.
set -eu
cd "$(dirname "$0")/.."
EXTRA="$*"
docker run --rm -v "$PWD":/src -w /src zk-kindlehf sh -c "
  set -e
  TC=\$HOME/x-tools/arm-kindlehf-linux-gnueabihf
  # FBInk must be the build tools/build-fbink.sh makes: with IMAGE (fbink_print_raw_data, the
  # Colorsoft's framebuffer color path; stb_image comes with it) and a device table that knows
  # the ColorSoft 2 (codename Julep). A stale MINIMAL library would link silently and leave the
  # Colorsoft gray.
  FBINK=build-deps/kindle/prefix/lib/libfbink.a
  [ -f \$FBINK ] || { echo \"build-kindle: \$FBINK missing: run tools/build-fbink.sh in the container\" >&2; exit 1; }
  grep -a -q Julep \$FBINK || { echo \"build-kindle: \$FBINK predates the ColorSoft 2 (no Julep): rebuild it with tools/build-fbink.sh\" >&2; exit 1; }
  \$TC/bin/arm-kindlehf-linux-gnueabihf-nm \$FBINK 2>/dev/null | grep -q ' T fbink_print_raw_data\$' &&
    \$TC/bin/arm-kindlehf-linux-gnueabihf-nm \$FBINK 2>/dev/null | grep -q ' stbi__' ||
    { echo \"build-kindle: \$FBINK was built without IMAGE=1 (no image support): rebuild it with tools/build-fbink.sh\" >&2; exit 1; }
  case '$EXTRA' in *spikes=true*) [ -f build-deps/kindle/prefix/lib/libcrengine.a ] || tools/build-crengine.sh kindle ;; esac
  if [ ! -d build-kindle ]; then
    meson setup build-kindle --cross-file \$TC/meson-crosscompile.txt -Dplatform=kindle -Dtests=false $EXTRA
  elif [ -n '$EXTRA' ]; then
    meson configure build-kindle $EXTRA
  fi
  ninja -C build-kindle
  # Stripped copies for the device (the debug builds carry ~40 MB of symbols).
  mkdir -p build-kindle/dist
  for f in zotero zk-sync libzk-epub.so libzk-pdf.so; do
    [ -f build-kindle/\$f ] && \$TC/bin/arm-kindlehf-linux-gnueabihf-strip -o build-kindle/dist/\$f build-kindle/\$f
  done
"
