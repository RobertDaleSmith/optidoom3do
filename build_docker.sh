#!/usr/bin/env bash
# Docker-based build for OptiDoom 3DO. Replaces the Windows .bat
# pipeline. Uses the trapexit/3do-devkit toolchain image (Norcroft
# ARM compilers + 3doiso + 3DOEncrypt under wine) and the
# trapexit/portfolio_os source release for SDK headers.
#
# Requires the user's own commercial Doom-3DO ISO at
# ISOdecompile/doom.iso (legal grey, can't be redistributed).

set -euo pipefail
cd "$(dirname "$0")"

IMAGE_TAG="joypad-tester-3do:latest"
TARGET="optidoom"

# Use a local portfolio_os fork if present so source edits flow through.
if [ -d "$HOME/git/portfolio_os/.git" ]; then
    PORTFOLIO_OS_DIR="$HOME/git/portfolio_os"
else
    PORTFOLIO_OS_DIR="$PWD/.portfolio_os"
    [ -d "$PORTFOLIO_OS_DIR/.git" ] || \
        git clone --depth 1 https://github.com/trapexit/portfolio_os.git "$PORTFOLIO_OS_DIR"
fi

case "${1:-build}" in
    build|"")
        if ! docker image inspect "$IMAGE_TAG" >/dev/null 2>&1; then
            echo "error: docker image '$IMAGE_TAG' not found." >&2
            echo "Build it from joypad-ai/joypad-tester (3do/buildtools/Dockerfile)." >&2
            exit 1
        fi

        mkdir -p build .wine-home

        docker run --rm \
            --platform=linux/amd64 \
            -v "$PWD:/work" \
            -v "$PORTFOLIO_OS_DIR:/po:ro" \
            -u "$(id -u):$(id -g)" \
            -e HOME=/work/.wine-home \
            "$IMAGE_TAG" \
            bash -c '
                set -e
                export PATH=/opt/3do-devkit/bin/compiler/linux:/opt/3do-devkit/bin/tools/linux:$PATH

                # ---- header compat dir ----
                # portfolio_os ships SDK headers all-lowercase but
                # OptiDoom-era code expects PascalCase (Types.h, etc.).
                # Build a compat dir of symlinks: capitalised-first-letter
                # for every SDK header, plus explicit PascalCase aliases
                # for multi-word Lib3DO headers (DisplayUtils.h, etc.).
                COMPAT=/tmp/inc_compat
                rm -rf $COMPAT && mkdir -p $COMPAT
                for dir in /po/src/kernel/includes /po/src/input/includes \
                           /po/src/filesystem/includes /po/src/graphics/includes \
                           /po/src/libs/lib3DO/includes /po/src/libs/operamath/includes \
                           /po/src/includes /po/src/audio/audiofolio/includes \
                           /po/src/audio/musiclib/includes /po/src/international/includes; do
                  [ -d "$dir" ] || continue
                  cd "$dir"
                  for f in *.h *.i; do
                    [ -f "$f" ] || continue
                    base=$(basename "$f" .h); base=$(basename "$base" .i)
                    # Skip headers OptiDoom bundles in lib/ (would conflict).
                    case "$base" in burger|intmath|3dlib|3DLib) continue ;; esac
                    cap=$(echo "$f" | sed -E "s/^(.)/\U\1/")
                    [ -e "$COMPAT/$cap" ] || ln -sf "$dir/$f" "$COMPAT/$cap"
                    [ -e "$COMPAT/$f"   ] || ln -sf "$dir/$f" "$COMPAT/$f"
                  done
                done
                # Multi-word PascalCase aliases that single-capital
                # transformation misses.
                L=/po/src/libs/lib3DO/includes
                F=/po/src/filesystem/includes
                for p in "$L/animutils.h:AnimUtils.h" "$L/blockfile.h:BlockFile.h" \
                         "$L/celutils.h:CelUtils.h" "$L/debug3do.h:Debug3DO.h" \
                         "$L/deletecelmagic.h:DeleteCelMagic.h" \
                         "$L/displayutils.h:DisplayUtils.h" "$L/fontlib.h:FontLib.h" \
                         "$L/form3do.h:Form3DO.h" "$L/init3do.h:Init3DO.h" \
                         "$L/init3do.h:Init3do.h" "$L/macros3do.h:Macros3DO.h" \
                         "$L/msgutils.h:MsgUtils.h" "$L/parse3do.h:Parse3DO.h" \
                         "$L/portfolio.h:Portfolio.h" "$L/textlib.h:TextLib.h" \
                         "$L/timerutils.h:TimerUtils.h" "$L/umemory.h:UMemory.h" \
                         "$L/utils3do.h:Utils3DO.h" \
                         "$F/filefunctions.h:FileFunctions.h"; do
                  s="${p%:*}"; a="${p#*:}"
                  [ -f "$s" ] && ln -sf "$s" "$COMPAT/$a"
                done

                # ---- compile ----
                cd /work/source
                rm -f *.o LaunchMe*
                make ARMDEV=/opt/3do-devkit 3DODEV=/opt/3do-devkit \
                     "INCPATH=-I../lib/3dlib -I../lib/burger -I../lib/intmath -I$COMPAT" \
                     CC=armcc AS=armasm LD=armlink MODBIN=modbin RM=rm \
                     LIBPATH=/opt/3do-devkit/lib/3do/ \
                     STARTUP=/opt/3do-devkit/lib/3do/cstartup.o 2>&1 | grep -E "^armcc|^armasm|^armlink|Error|error" | tail -20

                # The upstream makefiles links Lib3DO.lib (PascalCase),
                # but devkit ships lib3do.lib (lowercase). Also need
                # armlib.32b for ARM runtime helpers (_fadd, etc.) that
                # the upstream makefile does not list. Manually re-run
                # the link step with the right files.
                armlink -dupok -o LaunchMe. -aif -r -b 0x00 -sym LaunchMe.sym \
                  /opt/3do-devkit/lib/3do/cstartup.o \
                  ../lib/burger/burger.lib \
                  /opt/3do-devkit/lib/3do/lib3do.lib \
                  /opt/3do-devkit/lib/3do/operamath.lib \
                  /opt/3do-devkit/lib/3do/graphics.lib \
                  /opt/3do-devkit/lib/3do/audio.lib \
                  /opt/3do-devkit/lib/3do/music.lib \
                  /opt/3do-devkit/lib/3do/filesystem.lib \
                  /opt/3do-devkit/lib/3do/input.lib \
                  /opt/3do-devkit/lib/3do/swi.lib \
                  /opt/3do-devkit/lib/3do/clib.lib \
                  ../lib/string/string.lib \
                  ../lib/intmath/intmath.lib \
                  /opt/3do-devkit/lib/3do/armlib_cn.32b \
                  *.o 2>&1 | tail -5

                # modbin sets stack size on the binary. Original Makefile
                # calls "modbin 10000 LaunchMe" (positional, ancient ARM
                # SDT syntax). Modern toolchain uses --stack=N flag.
                modbin --stack=10000 LaunchMe. LaunchMe.

                # ---- ISO compose ----
                # Need ISOdecompile/CD/ populated with extracted Doom
                # asset files (user-provided commercial ISO). The
                # extract step happens out-of-docker (see below).
                if [ ! -d /work/ISOdecompile/CD ]; then
                  echo "warning: ISOdecompile/CD/ missing -- skipping ISO step."
                  echo "  Build LaunchMe is at source/LaunchMe."
                  echo "  Drop your commercial Doom 3DO ISO at"
                  echo "  ISOdecompile/doom.iso, run \"$0 extract\", then rebuild."
                  exit 0
                fi
                cp LaunchMe. /work/ISOdecompile/CD/LaunchMe
                cp -r /work/ISOdecompile/CDextra/. /work/ISOdecompile/CD/

                cd /work
                3doiso -in ISOdecompile/CD -out build/optidoom.iso
                3DOEncrypt genromtags build/optidoom.iso
            '
        echo "Built source/LaunchMe. -- iso at build/optidoom.iso if assets staged."
        ;;
    extract)
        if [ ! -f ISOdecompile/doom.iso ]; then
            echo "error: ISOdecompile/doom.iso missing. Drop your Doom 3DO ISO there." >&2
            exit 1
        fi
        docker run --rm --platform=linux/amd64 -v "$PWD:/work" "$IMAGE_TAG" \
            /opt/3do-devkit/bin/tools/linux/3doiso \
              -in /work/ISOdecompile/doom.iso \
              -out /work/ISOdecompile/CD
        echo "Extracted to ISOdecompile/CD/"
        ;;
    clean)
        rm -rf build/ ISOdecompile/CD/
        cd source && rm -f *.o LaunchMe* 2>/dev/null || true
        ;;
    *)
        echo "Usage: $0 [build|extract|clean]" >&2
        exit 1
        ;;
esac
