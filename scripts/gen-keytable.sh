#!/usr/bin/env bash
# Generate src/keys.inc from the kernel's input-event-codes header.
#
# The table is committed so a normal build needs no code generation; rerun this
# script only when the kernel UAPI header changes.
#
# Usage: scripts/gen-keytable.sh [HEADER [DESTINATION]]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
header="${1:-/usr/include/linux/input-event-codes.h}"
destination="${2:-$root/src/keys.inc}"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/loomwright-keytable.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT

# Collect name/code pairs in header order plus the first name for each code.
# The pattern matches the KEY_ defines that matter: an uppercase name and
# either a 0x-prefixed or a decimal value at the start of a #define line.
LC_ALL=C awk -v pairs="$tmp/pairs.tsv" -v reverse="$tmp/reverse.tsv" '
function decimal(value,    i, digit, result) {
    if (substr(value, 1, 2) == "0x") {
        value = substr(value, 3)
        result = 0
        for (i = 1; i <= length(value); ++i) {
            digit = index("0123456789abcdef", tolower(substr(value, i, 1))) - 1
            result = result * 16 + digit
        }
        return result
    }
    return value + 0
}
/^#define[ \t]+KEY_[A-Z0-9_]+[ \t]+(0x[0-9a-fA-F]+|[0-9]+)/ {
    line = $0
    sub(/^#define[ \t]+KEY_/, "", line)
    name = line
    sub(/[ \t].*$/, "", name)
    value = line
    sub(/^[^ \t]+[ \t]+/, "", value)
    # Keep only the part of the value that the pattern actually matches, so a
    # malformed 0x token falls back to its leading decimal digits.
    if (match(value, /^0x[0-9a-fA-F]+/)) value = substr(value, 1, RLENGTH)
    else if (match(value, /^[0-9]+/)) value = substr(value, 1, RLENGTH)
    code = decimal(value)
    printf "%s\t%d\n", name, code > pairs
    if (!(code in first)) {
        printf "%d\t%s\n", code, name > reverse
        first[code] = name
    }
}
' "$header"

count=$(wc -l < "$tmp/pairs.tsv")
if (( count < 300 )); then
    printf 'only %d KEY_ definitions found in %s\n' "$count" "$header" >&2
    exit 1
fi
codes=$(wc -l < "$tmp/reverse.tsv")

# Names sort lexicographically (C collation, as a tuple sort does), with the
# code as a numeric tie-breaker. Reverse entries sort by code.
LC_ALL=C sort -t $'\t' -k1,1 -k2,2n "$tmp/pairs.tsv" > "$tmp/sorted-pairs.tsv"
LC_ALL=C sort -t $'\t' -k1,1n "$tmp/reverse.tsv" > "$tmp/sorted-reverse.tsv"

{
    printf '%s\n' \
        '// Generated from <linux/input-event-codes.h>. Do not edit by hand.' \
        '// Regenerate with scripts/gen-keytable.sh.' \
        '//' \
        '// kKeyTable is sorted by name for lookup; kKeyReverse is sorted by code and' \
        '// keeps the first name defined for a code, which is the canonical one' \
        '// (KEY_ESC for 1, KEY_LEFTCTRL for 29, and so on).' \
        ''
    printf '%s\n' 'static const KeyEntry kKeyTable[] = {'
    LC_ALL=C awk -F '\t' '{ printf "    {\"%s\", %s},\n", $1, $2 }' "$tmp/sorted-pairs.tsv"
    printf '%s\n' '};' ''
    printf '%s\n' 'static const ReverseKeyEntry kKeyReverse[] = {'
    LC_ALL=C awk -F '\t' '{ printf "    {%s, \"%s\"},\n", $1, $2 }' "$tmp/sorted-reverse.tsv"
    printf '%s\n' '};'
} > "$tmp/keys.inc"

mv "$tmp/keys.inc" "$destination"
printf 'wrote %s: %d names, %d codes\n' "$destination" "$count" "$codes"
