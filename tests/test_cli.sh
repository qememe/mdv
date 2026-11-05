#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
set -eu
case ${1:-./mdv} in
    /*) bin=${1:-./mdv} ;;
    *) bin="$PWD/${1:-./mdv}" ;;
esac
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM

fail() { echo "FAIL: $*" >&2; exit 1; }
expect_status() {
    expected=$1
    shift
    actual=0
    "$bin" "$@" > "$scratch/out" 2> "$scratch/err" || actual=$?
    [ "$actual" -eq "$expected" ] || fail "exit $actual, expected $expected: $*"
}
contains() { grep -F -- "$1" "$scratch/out" > /dev/null || fail "missing: $1"; }

expect_status 0 --version
contains 'mdv 1.0.0'
expect_status 0 --help
contains 'mdv [параметры]'
for width in 0 19 abc 999999999999999999999; do
    expect_status 2 --width "$width"
done
expect_status 2 --theme unknown
expect_status 2 --unknown
expect_status 2 one.md two.md
expect_status 1 "$scratch/missing.md"
expect_status 1 "$scratch"
printf '\377\376' > "$scratch/invalid.md"
expect_status 1 "$scratch/invalid.md"

printf '\357\273\277# Привет\n\n**Текст** &amp; &#x1F600;\n' > "$scratch/Мой файл.md"
expect_status 0 --no-pager "$scratch/Мой файл.md"
contains 'Привет'
contains 'Текст & 😀'
if grep -F '**Текст**' "$scratch/out" > /dev/null; then fail 'unparsed emphasis'; fi
if LC_ALL=C grep "$(printf '\033')" "$scratch/out" > /dev/null; then fail 'ANSI in pipe'; fi
printf '# stdin\n\nText\n' | "$bin" - > "$scratch/out"
contains 'stdin'
printf 'Implicit stdin\n' | "$bin" > "$scratch/out"
contains 'Implicit stdin'
: > "$scratch/empty.md"
expect_status 0 "$scratch/empty.md"
cat > "$scratch/blocks.md" <<'MARKDOWN'
# Heading

- **First** item
  - Nested item
- Second item

3. Ordered item
4. Another item

> Quote with *emphasis*

[Link](https://example.org?a=1&amp;b=2)

| Column | Value |
| --- | ---: |
| abcdefghijklmnopqrstuvwxyz | Right |
| Unicode | 中文 😀 |

```c
int main(void) {
    return 42;
}
```
MARKDOWN
expect_status 0 --width 40 "$scratch/blocks.md"
contains '• First item'
contains '• Nested item'
contains '3. Ordered item'
contains '│ Quote with emphasis'
contains 'https://example.org?a=1&b=2'
contains 'int main(void)'
contains 'return 42;'
contains '中文 😀'
expect_status 0 --width 20 "$scratch/blocks.md"
contains 'Right'
# Deeply nested hostile input must fail without overflowing the C stack.
awk 'BEGIN { for(i=0;i<150;i++) printf "> "; print "deep" }' > "$scratch/deep.md"
expect_status 1 "$scratch/deep.md"
# Document bytes must not be able to inject ANSI controls into the terminal.
printf 'hello\033[31m world\n' | "$bin" --no-color > "$scratch/out"
if LC_ALL=C grep "$(printf '\033')" "$scratch/out" > /dev/null; then fail 'input ANSI injection'; fi
printf 'OK: CLI, UTF-8, stdin, Markdown, narrow tables, nesting, terminal controls\n'
