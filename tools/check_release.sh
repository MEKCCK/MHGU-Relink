#!/bin/sh
# Fails if an artifact carries recompiled game code (libmhgugame.so / mhgugame.so) or the game's data.
# Those are derived from the player's own dump: they are built locally (tools/build_game.py) and never
# distributed. Run it on every APK / package before publishing.
#
#   tools/check_release.sh *.apk dist/*.pkg.tar.zst dist/*.tar.gz
set -u
bad=0
for f in "$@"; do
  [ -e "$f" ] || continue
  case "$f" in
    *.apk)
      hits=$(unzip -l "$f" 2>/dev/null | awk '{print $4}' | grep -E '(^|/)libmhgugame\.so$|(^|/)mhgugame\.so$' )
      ;;
    *.pkg.tar.*|*.tar.gz|*.tar.xz|*.tar.zst)
      hits=$(tar -tf "$f" 2>/dev/null | grep -E '(^|/)libmhgugame\.so$|(^|/)mhgugame\.so$|(^|/)(exefs|romfs)/' )
      ;;
    *) continue ;;
  esac
  if [ -n "$hits" ]; then
    echo "FAIL $f"
    echo "$hits" | sed 's/^/     /'
    bad=1
  else
    echo "ok   $f"
  fi
done
exit $bad
