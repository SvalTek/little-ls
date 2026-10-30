// End-to-end test for the little-ls language server over stdio.
// Run with: node tests/lsp.mjs <path-to-little-ls>
import { spawn } from "node:child_process";
import assert from "node:assert/strict";

const executable = process.argv[2];
if (!executable) {
  console.error("usage: node tests/lsp.mjs <little-ls executable>");
  process.exit(2);
}

const server = spawn(executable, ["--stdio"], { stdio: ["pipe", "pipe", "pipe"] });

let stderr = "";
server.stderr.on("data", (chunk) => { stderr += chunk.toString("utf8"); });

let buffer = Buffer.alloc(0);
const queued = [];
const waiters = [];

server.stdout.on("data", (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);

  for (;;) {
    const separator = buffer.indexOf("\r\n\r\n");
    if (separator < 0) return;

    const header = buffer.subarray(0, separator).toString("ascii");
    const match = /Content-Length:\s*(\d+)/i.exec(header);
    if (!match) {
      buffer = buffer.subarray(separator + 4);
      continue;
    }

    const length = Number(match[1]);
    if (buffer.length < separator + 4 + length) return;

    const body = buffer.subarray(separator + 4, separator + 4 + length).toString("utf8");
    buffer = buffer.subarray(separator + 4 + length);

    const message = JSON.parse(body);
    const waiter = waiters.shift();
    if (waiter) waiter(message);
    else queued.push(message);
  }
});

function nextMessage(timeout) {
  if (queued.length) return Promise.resolve(queued.shift());
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("timed out waiting for a message")), timeout);
    waiters.push((message) => { clearTimeout(timer); resolve(message); });
  });
}

async function waitFor(predicate, timeout = 5000) {
  const deadline = Date.now() + timeout;
  for (;;) {
    const message = await nextMessage(Math.max(1, deadline - Date.now()));
    if (predicate(message)) return message;
  }
}

function send(message) {
  const json = JSON.stringify(message);
  const payload = Buffer.from(json, "utf8");
  server.stdin.write(`Content-Length: ${payload.length}\r\n\r\n`);
  server.stdin.write(payload);
}

const closed = new Promise((resolve) => server.on("close", (code) => resolve(code)));

async function main() {
  send({ jsonrpc: "2.0", id: 1, method: "initialize", params: { capabilities: {}, processId: process.pid, rootUri: null } });
  const initialize = await waitFor((message) => message.id === 1);
  assert.equal(initialize.result.capabilities.textDocumentSync, 1);
  assert.equal(initialize.result.capabilities.documentSymbolProvider, true);

  send({ jsonrpc: "2.0", method: "initialized", params: {} });

  const uri = "file:///test/main.little";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri, languageId: "little", version: 1, text: "var x = 1\nvar 2 = 3\n" } }
  });

  const invalid = await waitFor((message) => message.method === "textDocument/publishDiagnostics");
  assert.equal(invalid.params.uri, uri);
  assert.equal(invalid.params.diagnostics.length, 1, "expected exactly one diagnostic");
  assert.equal(invalid.params.diagnostics[0].range.start.line, 1);
  assert.equal(invalid.params.diagnostics[0].range.start.character, 4);
  assert.equal(invalid.params.diagnostics[0].range.end.character, 5);
  assert.equal(invalid.params.diagnostics[0].severity, 1);
  assert.match(invalid.params.diagnostics[0].message, /Expected identifier/);

  send({
    jsonrpc: "2.0",
    method: "textDocument/didChange",
    params: { textDocument: { uri, version: 2 }, contentChanges: [{ text: "var x = 1\n" }] }
  });

  const fixed = await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.version === 2);
  assert.equal(fixed.params.diagnostics.length, 0, "diagnostics were not cleared");

  send({ jsonrpc: "2.0", id: 2, method: "textDocument/documentSymbol", params: { textDocument: { uri } } });
  const symbols = await waitFor((message) => message.id === 2);
  assert.equal(symbols.result.length, 1);
  assert.equal(symbols.result[0].name, "x");
  assert.equal(symbols.result[0].kind, 13);
  assert.equal(symbols.result[0].range.start.line, 0);
  assert.equal(symbols.result[0].range.start.character, 4);

  const classUri = "file:///test/klass.little";
  const classSource = "class Point {\n  x\n  length() {\n    return 1\n  }\n}\n";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: classUri, languageId: "little", version: 1, text: classSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === classUri);

  send({ jsonrpc: "2.0", id: 3, method: "textDocument/documentSymbol", params: { textDocument: { uri: classUri } } });
  const classSymbols = await waitFor((message) => message.id === 3);
  assert.equal(classSymbols.result.length, 1);
  assert.equal(classSymbols.result[0].name, "Point");
  assert.equal(classSymbols.result[0].kind, 5);
  const children = classSymbols.result[0].children ?? [];
  assert.deepEqual(children.map((child) => [child.name, child.kind]), [["x", 8], ["length", 6]]);

  send({ jsonrpc: "2.0", id: 4, method: "shutdown" });
  const shutdown = await waitFor((message) => message.id === 4);
  assert.equal(shutdown.result, null);

  send({ jsonrpc: "2.0", method: "exit" });
  const code = await closed;
  assert.equal(code, 0, "server exited with a failure code");
  assert.equal(stderr, "", "server wrote to stderr");
}

main().then(
  () => { console.log("lsp tests passed"); },
  (error) => {
    console.error(error);
    server.kill();
    process.exit(1);
  }
);
