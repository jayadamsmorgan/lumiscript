#!/bin/sh
set -eu

builddir="$1"
server="$builddir/lumils"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

msg_init='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"capabilities":{}}}'
msg_open='{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///test.lumi","languageId":"lumi","version":1,"text":"type static\n\nrender {\n    color rgb(1, 2, 3)\n}\n"}}}'
msg_hover='{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///test.lumi"},"position":{"line":3,"character":10}}}'
msg_completion='{"jsonrpc":"2.0","id":3,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///test.lumi"},"position":{"line":3,"character":10}}}'
msg_symbols='{"jsonrpc":"2.0","id":4,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///test.lumi"}}}'
msg_shutdown='{"jsonrpc":"2.0","id":5,"method":"shutdown","params":{}}'
msg_exit='{"jsonrpc":"2.0","method":"exit","params":{}}'

send() {
    payload="$1"
    printf 'Content-Length: %s\r\n\r\n%s' "$(printf '%s' "$payload" | wc -c | tr -d ' ')" "$payload"
}

{
    send "$msg_init"
    send "$msg_open"
    send "$msg_hover"
    send "$msg_completion"
    send "$msg_symbols"
    send "$msg_shutdown"
    send "$msg_exit"
} | "$server" >"$tmpdir/out.txt"

grep -Fq '"hoverProvider":true' "$tmpdir/out.txt"
grep -Fq 'publishDiagnostics' "$tmpdir/out.txt"
grep -Fq 'rgb(r, g, b)' "$tmpdir/out.txt"
grep -Fq '"label":"rgb"' "$tmpdir/out.txt"
grep -Fq '"label":"floor"' "$tmpdir/out.txt"
grep -Fq '"label":"rand"' "$tmpdir/out.txt"
grep -Fq '"name":"render"' "$tmpdir/out.txt"
