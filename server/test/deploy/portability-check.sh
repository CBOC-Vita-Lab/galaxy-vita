#!/bin/bash
# Sourced by deploy-script-test.sh and zone-script-test.sh. (1) portability_scan: greps scripts/*.sh and test/deploy/* (this file excluded: it
# names the forms) for GNU- or Linux-only forms that break on macOS (/bin/bash 3.2, BSD userland). (2) make_mac_shim DIR: a PATH directory whose
# stat / readlink / date / grep / xargs / base64 / find / sed / cp refuse the GNU-only forms (they exec the real tool otherwise); the tests run a
# second time with it (MAC_SHIM=1), the mock also makes /proc unusable (MOCK_NO_PROC=1).
# Allowlist: "<file-name>|<fixed text on the line>|<reason>" (a line that contains the fixed text in that file is not reported).
PORT_ALLOW=(
)
# ERE patterns (one per line): name|regex
PORT_PATTERNS=(
  'stat -c / stat -f (GNU/BSD differ)|(^|[^A-Za-z_./-])stat -'
  '/proc/|/proc/'
  'readlink -f|readlink -f'
  'date -d|date -d'
  'find -printf|find .*-printf'
  'mapfile|(^|[^A-Za-z_])mapfile( |$)'
  'readarray|(^|[^A-Za-z_])readarray( |$)'
  'declare -A|declare -A'
  '${x,,} / ${x^^} (bash 4)|[$][{][A-Za-z_0-9]+(,,|[\^][\^])'
  'grep -P|grep( -[A-Za-z]+)* -[A-Za-z]*P'
  'xargs -r|xargs( -[A-Za-z]+)* -r'
  'cp/ln/mv long options|(^|[^A-Za-z_./-])(cp|mv|ln|rm) (-[A-Za-z]+ )*--[a-z]'
  'base64 -w|base64 -w'
  'sed -r|sed( -[A-Za-z]+)* -r( |$)'
  'sort -V|sort( -[A-Za-z]+)* -V'
  'head/tail -n -N|(head|tail) -n? ?-[0-9]'
  'date +%N|date .*%N'
)
portability_scan() { # $1 = server dir; prints offending lines, returns 1 if any
  local srv=$1 f line pat name rx bad=0 base allowed a
  for f in "$srv"/scripts/*.sh "$srv"/test/deploy/*; do
    [ -f "$f" ] || continue; base=$(basename "$f"); [ "$base" = portability-check.sh ] && continue
    for pat in "${PORT_PATTERNS[@]}"; do
      name=${pat%%|*}; rx=${pat#*|}
      while IFS= read -r line; do
        [ -n "$line" ] || continue
        allowed=0
        for a in ${PORT_ALLOW[@]+"${PORT_ALLOW[@]}"}; do
          [ "${a%%|*}" = "$base" ] || continue; a=${a#*|}
          case "$line" in *"${a%%|*}"*) allowed=1 ;; esac
        done
        [ $allowed -eq 1 ] || { echo "portability: $base: $name: $line" | cut -c1-200; bad=1; }
      done < <(grep -E -- "$rx" "$f" | grep -vE '^[[:space:]]*#')
    done
    # sed -i with no backup suffix (GNU only; BSD needs -i '')
    while IFS= read -r line; do
      [ -n "$line" ] || continue; echo "portability: $base: sed -i without a suffix: $line" | cut -c1-200; bad=1
    done < <(grep -E 'sed -i( |$)' "$f" | grep -vE "sed -i (''|\"\")" | grep -vE '^[[:space:]]*#')
    # sed with a BRE alternation \| (GNU only; -E lines are fine)
    while IFS= read -r line; do
      [ -n "$line" ] || continue; echo "portability: $base: sed BRE alternation: $line" | cut -c1-200; bad=1
    done < <(grep -E "sed [^|]*\\\\\\|" "$f" | grep -vE 'sed (-n )?-E|sed -E|-E -e' | grep -vE '^[[:space:]]*#')
  done
  return $bad
}
make_mac_shim() { # $1 = dir
  local d=$1 t real
  mkdir -p "$d"
  for t in stat readlink date grep xargs base64 find sed cp mv ln; do
    real=$(command -v "$t") || continue
    cat > "$d/$t" <<S
#!/bin/bash
real="$real"
for a in "\$@"; do
  case "$t:\$a" in
    stat:-c|stat:-c*|stat:--format*|stat:--printf*) echo "stat: illegal option -- c (mac shim)" >&2; exit 1 ;;
    readlink:-f) echo "readlink: illegal option -- f (mac shim)" >&2; exit 1 ;;
    date:-d|date:--date*) echo "date: illegal option -- d (mac shim)" >&2; exit 1 ;;
    grep:-P|grep:-*P*) case "\$a" in --*) ;; -[A-Za-z]*P*) echo "grep: invalid option -- P (mac shim)" >&2; exit 2 ;; esac ;;
    xargs:-r|xargs:--no-run-if-empty) echo "xargs: illegal option -- r (mac shim)" >&2; exit 1 ;;
    base64:-w|base64:-w*) echo "base64: invalid option -- w (mac shim)" >&2; exit 1 ;;
    find:-printf) echo "find: -printf: unknown primary or operator (mac shim)" >&2; exit 1 ;;
    sed:-r|sed:--regexp-extended) echo "sed: illegal option -- r (mac shim)" >&2; exit 1 ;;
    cp:--*|mv:--*|ln:--*) echo "$t: illegal option \$a (mac shim)" >&2; exit 1 ;;
  esac
done
if [ "$t" = sed ]; then # sed -i with no suffix argument: BSD takes the next word as the suffix
  for a in "\$@"; do case "\$a" in -i) echo "sed: -i needs a suffix argument (mac shim)" >&2; exit 1 ;; esac; done
fi
exec "\$real" "\$@"
S
    chmod +x "$d/$t"
  done
}
