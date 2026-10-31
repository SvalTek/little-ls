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
    if (process.env.LSP_DEBUG) console.error("recv", message.id ?? message.method);
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

async function waitFor(predicate, label, timeout = 5000) {
  const deadline = Date.now() + timeout;
  for (;;) {
    let message;
    try {
      message = await nextMessage(Math.max(1, deadline - Date.now()));
    } catch (error) {
      throw new Error(`${label}: ${error.message}`);
    }
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
  const initialize = await waitFor((message) => message.id === 1, "initialize");
  assert.equal(initialize.result.capabilities.textDocumentSync, 1);
  assert.equal(initialize.result.capabilities.documentSymbolProvider, true);

  send({ jsonrpc: "2.0", method: "initialized", params: {} });

  const uri = "file:///test/main.little";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri, languageId: "little", version: 1, text: "var x = 1\nvar 2 = 3\n" } }
  });

  const invalid = await waitFor((message) => message.method === "textDocument/publishDiagnostics", "open diagnostics");
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

  const fixed = await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.version === 2, "cleared diagnostics");
  assert.equal(fixed.params.diagnostics.length, 0, "diagnostics were not cleared");

  send({ jsonrpc: "2.0", id: 2, method: "textDocument/documentSymbol", params: { textDocument: { uri } } });
  const symbols = await waitFor((message) => message.id === 2, "symbols");
  assert.equal(symbols.result.length, 1);
  assert.equal(symbols.result[0].name, "x");
  assert.equal(symbols.result[0].kind, 13);
  assert.equal(symbols.result[0].range.start.line, 0);
  assert.equal(symbols.result[0].range.start.character, 0);
  assert.equal(symbols.result[0].range.end.line, 0);
  assert.equal(symbols.result[0].range.end.character, 9);
  assert.equal(symbols.result[0].selectionRange.start.character, 4);
  assert.equal(symbols.result[0].selectionRange.end.character, 5);

  const classUri = "file:///test/klass.little";
  const classSource = "class Point {\n  x\n  length() {\n    return 1\n  }\n}\n";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: classUri, languageId: "little", version: 1, text: classSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === classUri, "class diagnostics");

  send({ jsonrpc: "2.0", id: 3, method: "textDocument/documentSymbol", params: { textDocument: { uri: classUri } } });
  const classSymbols = await waitFor((message) => message.id === 3, "class symbols");
  assert.equal(classSymbols.result.length, 1);
  assert.equal(classSymbols.result[0].name, "Point");
  assert.equal(classSymbols.result[0].kind, 5);
  assert.equal(classSymbols.result[0].range.end.line, 5);
  assert.equal(classSymbols.result[0].range.end.character, 1);
  const children = classSymbols.result[0].children ?? [];
  assert.deepEqual(children.map((child) => [child.name, child.kind]), [["x", 8], ["length", 6]]);
  assert.equal(children[1].range.start.line, 2);
  assert.equal(children[1].range.start.character, 2);
  assert.equal(children[1].range.end.line, 4);
  assert.equal(children[1].range.end.character, 3);

  const operatorUri = "file:///test/operator.little";
  const operatorSource = "var x = 1\n/\nclass Point {\n  x\n}\n";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: operatorUri, languageId: "little", version: 1, text: operatorSource } }
  });
  const malformed = await waitFor(
    (message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === operatorUri,
    "operator diagnostics"
  );
  assert.equal(malformed.params.diagnostics.length, 1);
  assert.match(malformed.params.diagnostics[0].message, /Malformed expression/);
  assert.equal(malformed.params.diagnostics[0].range.start.line, 1);
  assert.equal(malformed.params.diagnostics[0].range.start.character, 0);
  assert.equal(malformed.params.diagnostics[0].range.end.character, 1);

  // The server must stay alive after a malformed expression (this used to crash
  // the parser when an operator had no operands), and analysis must resume so
  // declarations after the broken statement are still visible.
  send({ jsonrpc: "2.0", id: 4, method: "textDocument/documentSymbol", params: { textDocument: { uri: operatorUri } } });
  const operatorSymbols = await waitFor((message) => message.id === 4, "operator symbols");
  assert.deepEqual(operatorSymbols.result.map((symbol) => [symbol.name, symbol.kind]), [["Point", 5]]);

  const docsUri = "file:///test/docs.little";
  const docsSource = [
    ";; Adds one to a counter.",
    ";; @param number amount [1] how much to add",
    ";; @return number the new total",
    "fn increment(amount) {",
    "  return amount",
    "}",
    "",
    "var count = 0",
    "io.print(increment(count))",
    "",
  ].join("\n");
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: docsUri, languageId: "little", version: 1, text: docsSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === docsUri, "docs diagnostics");

  send({ jsonrpc: "2.0", id: 6, method: "textDocument/hover", params: { textDocument: { uri: docsUri }, position: { line: 8, character: 12 } } });
  const functionHover = await waitFor((message) => message.id === 6, "function hover");
  assert.match(functionHover.result.contents.value, /fn increment\(amount\)/);
  assert.match(functionHover.result.contents.value, /Adds one to a counter/);
  assert.match(functionHover.result.contents.value, /how much to add/);

  send({ jsonrpc: "2.0", id: 7, method: "textDocument/definition", params: { textDocument: { uri: docsUri }, position: { line: 8, character: 19 } } });
  const countDefinition = await waitFor((message) => message.id === 7, "count definition");
  assert.equal(countDefinition.result.uri, docsUri);
  assert.equal(countDefinition.result.range.start.line, 7);
  assert.equal(countDefinition.result.range.start.character, 4);
  assert.equal(countDefinition.result.range.end.character, 9);

  send({ jsonrpc: "2.0", id: 8, method: "textDocument/hover", params: { textDocument: { uri: docsUri }, position: { line: 8, character: 19 } } });
  const variableHover = await waitFor((message) => message.id === 8, "variable hover");
  assert.match(variableHover.result.contents.value, /var count/);

  const memberUri = "file:///test/member.little";
  const memberSource = "class Box {\n  value\n  read() {\n    return @value\n  }\n}\n";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: memberUri, languageId: "little", version: 1, text: memberSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === memberUri, "member diagnostics");

  send({ jsonrpc: "2.0", id: 9, method: "textDocument/hover", params: { textDocument: { uri: memberUri }, position: { line: 3, character: 13 } } });
  const memberHover = await waitFor((message) => message.id === 9, "member hover");
  assert.match(memberHover.result.contents.value, /field value/);

  const withUri = "file:///test/with.little";
  const withSource = [
    "var config = { baseUrl: \"localhost\" }",
    "with config {",
    "  io.print(@baseUrl)",
    "}",
    "",
  ].join("\n");
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: withUri, languageId: "little", version: 1, text: withSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === withUri, "with diagnostics");

  send({ jsonrpc: "2.0", id: 10, method: "textDocument/hover", params: { textDocument: { uri: withUri }, position: { line: 2, character: 13 } } });
  const withHover = await waitFor((message) => message.id === 10, "with hover");
  assert.match(withHover.result.contents.value, /field baseUrl/);

  send({ jsonrpc: "2.0", id: 11, method: "textDocument/definition", params: { textDocument: { uri: withUri }, position: { line: 2, character: 13 } } });
  const withDefinition = await waitFor((message) => message.id === 11, "with definition");
  assert.equal(withDefinition.result.range.start.line, 0);
  assert.equal(withDefinition.result.range.start.character, 15);
  assert.equal(withDefinition.result.range.end.character, 22);

  const nestedUri = "file:///test/nested.little";
  const nestedSource = [
    "with { a: 1 } {",
    "  with { b: 2 } {",
    "    io.print(@b)",
    "  }",
    "}",
    "",
  ].join("\n");
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: nestedUri, languageId: "little", version: 1, text: nestedSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === nestedUri, "nested diagnostics");

  send({ jsonrpc: "2.0", id: 12, method: "textDocument/hover", params: { textDocument: { uri: nestedUri }, position: { line: 2, character: 14 } } });
  const nestedHover = await waitFor((message) => message.id === 12, "nested hover");
  assert.match(nestedHover.result.contents.value, /field b/);

  const unknownUri = "file:///test/unknown.little";
  const unknownSource = "fn f(config) {\n  with config {\n    io.print(@baseUrl)\n  }\n}\n";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: unknownUri, languageId: "little", version: 1, text: unknownSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === unknownUri, "unknown diagnostics");

  send({ jsonrpc: "2.0", id: 13, method: "textDocument/hover", params: { textDocument: { uri: unknownUri }, position: { line: 2, character: 14 } } });
  const unknownHover = await waitFor((message) => message.id === 13, "unknown hover");
  assert.equal(unknownHover.result, null);

  const boxUri = "file:///test/box.little";
  const boxSource = [
    "class Box {",
    "  value",
    "  test() {",
    "    return 1",
    "  }",
    "}",
    "var p = Box()",
    "io.print(p.value)",
    "io.print(p:test())",
    "",
  ].join("\n");
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: boxUri, languageId: "little", version: 1, text: boxSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === boxUri, "box diagnostics");

  send({ jsonrpc: "2.0", id: 15, method: "textDocument/hover", params: { textDocument: { uri: boxUri }, position: { line: 6, character: 4 } } });
  const instanceHover = await waitFor((message) => message.id === 15, "instance hover");
  assert.match(instanceHover.result.contents.value, /var p: Box/);

  send({ jsonrpc: "2.0", id: 16, method: "textDocument/hover", params: { textDocument: { uri: boxUri }, position: { line: 7, character: 12 } } });
  const fieldHover = await waitFor((message) => message.id === 16, "field hover");
  assert.match(fieldHover.result.contents.value, /field value/);

  send({ jsonrpc: "2.0", id: 17, method: "textDocument/hover", params: { textDocument: { uri: boxUri }, position: { line: 8, character: 12 } } });
  const methodHover = await waitFor((message) => message.id === 17, "method hover");
  assert.match(methodHover.result.contents.value, /fn test\(\)/);

  send({ jsonrpc: "2.0", id: 18, method: "textDocument/definition", params: { textDocument: { uri: boxUri }, position: { line: 8, character: 12 } } });
  const methodDefinition = await waitFor((message) => message.id === 18, "method definition");
  assert.equal(methodDefinition.result.range.start.line, 2);
  assert.equal(methodDefinition.result.range.start.character, 2);
  assert.equal(methodDefinition.result.range.end.character, 6);

  const flowUri = "file:///test/flow.little";
  const flowSource = "var x = 1\nio.print(x)\nx = \"s\"\nio.print(x)\n";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: flowUri, languageId: "little", version: 1, text: flowSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === flowUri, "flow diagnostics");

  send({ jsonrpc: "2.0", id: 19, method: "textDocument/hover", params: { textDocument: { uri: flowUri }, position: { line: 1, character: 9 } } });
  const numberHover = await waitFor((message) => message.id === 19, "number hover");
  assert.match(numberHover.result.contents.value, /var x: number/);

  send({ jsonrpc: "2.0", id: 20, method: "textDocument/hover", params: { textDocument: { uri: flowUri }, position: { line: 3, character: 9 } } });
  const stringHover = await waitFor((message) => message.id === 20, "string hover");
  assert.match(stringHover.result.contents.value, /var x: string/);

  const namedUri = "file:///test/named.little";
  const namedSource = "class Named {\n  constructor(@label) {\n  }\n}\nvar n = Named(\"x\")\nio.print(n.label)\n";
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: namedUri, languageId: "little", version: 1, text: namedSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === namedUri, "named diagnostics");

  send({ jsonrpc: "2.0", id: 22, method: "textDocument/hover", params: { textDocument: { uri: namedUri }, position: { line: 5, character: 12 } } });
  const implicitHover = await waitFor((message) => message.id === 22, "implicit field hover");
  assert.match(implicitHover.result.contents.value, /field label/);

  const annotatedUri = "file:///test/annotated.little";
  const annotatedSource = [
    "class Box {",
    "  value",
    "}",
    ";; @param Box box",
    "fn use(box) {",
    "  with box {",
    "    io.print(@value)",
    "  }",
    "}",
    ";; @type Box",
    "var x = make()",
    "io.print(x.value)",
    ";; @return string",
    "fn label() {",
    "}",
    "var s = label()",
    "",
  ].join("\n");
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: annotatedUri, languageId: "little", version: 1, text: annotatedSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === annotatedUri, "annotated diagnostics");

  send({ jsonrpc: "2.0", id: 24, method: "textDocument/hover", params: { textDocument: { uri: annotatedUri }, position: { line: 6, character: 15 } } });
  const annotatedMember = await waitFor((message) => message.id === 24, "annotated member hover");
  assert.match(annotatedMember.result.contents.value, /field value/);

  send({ jsonrpc: "2.0", id: 25, method: "textDocument/hover", params: { textDocument: { uri: annotatedUri }, position: { line: 10, character: 4 } } });
  const annotatedVariable = await waitFor((message) => message.id === 25, "annotated variable hover");
  assert.match(annotatedVariable.result.contents.value, /var x: Box/);

  send({ jsonrpc: "2.0", id: 26, method: "textDocument/hover", params: { textDocument: { uri: annotatedUri }, position: { line: 15, character: 4 } } });
  const annotatedReturn = await waitFor((message) => message.id === 26, "annotated return hover");
  assert.match(annotatedReturn.result.contents.value, /var s: string/);

  const inheritedUri = "file:///test/inherited.little";
  const inheritedSource = [
    "class Base {",
    "  baseValue",
    "  baseMethod() {",
    "    return 1",
    "  }",
    "}",
    "class Child extends Base {",
    "}",
    "var c = Child()",
    "io.print(c.baseValue)",
    "io.print(c:baseMethod())",
    "",
  ].join("\n");
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: inheritedUri, languageId: "little", version: 1, text: inheritedSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === inheritedUri, "inherited diagnostics");

  send({ jsonrpc: "2.0", id: 28, method: "textDocument/hover", params: { textDocument: { uri: inheritedUri }, position: { line: 9, character: 12 } } });
  const inheritedField = await waitFor((message) => message.id === 28, "inherited field hover");
  assert.match(inheritedField.result.contents.value, /field baseValue/);

  send({ jsonrpc: "2.0", id: 29, method: "textDocument/hover", params: { textDocument: { uri: inheritedUri }, position: { line: 10, character: 12 } } });
  const inheritedMethod = await waitFor((message) => message.id === 29, "inherited method hover");
  assert.match(inheritedMethod.result.contents.value, /fn baseMethod\(\)/);

  const shapeUri = "file:///test/shape.little";
  const shapeSource = [
    ";; @class Options",
    ";; @field boolean foo should we enable foo?",
    ";; @field string name a label",
    "",
    ";; Configure the thing.",
    ";; @param Options options [{ foo: true }] method options",
    "fn configure(options) {",
    "  io.print(options.foo)",
    "  with options {",
    "    io.print(@name)",
    "  }",
    "}",
    "",
  ].join("\n");
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: shapeUri, languageId: "little", version: 1, text: shapeSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === shapeUri, "shape diagnostics");

  send({ jsonrpc: "2.0", id: 31, method: "textDocument/hover", params: { textDocument: { uri: shapeUri }, position: { line: 7, character: 20 } } });
  const shapeField = await waitFor((message) => message.id === 31, "shape field hover");
  assert.match(shapeField.result.contents.value, /field foo: bool/);
  assert.match(shapeField.result.contents.value, /should we enable foo/);

  send({ jsonrpc: "2.0", id: 32, method: "textDocument/hover", params: { textDocument: { uri: shapeUri }, position: { line: 9, character: 15 } } });
  const shapeReceiver = await waitFor((message) => message.id === 32, "shape receiver hover");
  assert.match(shapeReceiver.result.contents.value, /field name: string/);

  send({ jsonrpc: "2.0", id: 33, method: "textDocument/hover", params: { textDocument: { uri: shapeUri }, position: { line: 6, character: 14 } } });
  const shapeParam = await waitFor((message) => message.id === 33, "shape param hover");
  assert.match(shapeParam.result.contents.value, /param options: Options/);

  assert.doesNotMatch(shapeField.result.contents.value, /class Options/);

  const linkedUri = "file:///test/linked.little";
  const linkedSource = [
    ";; @class Known",
    ";; @field number size the size",
    "class Known {",
    "  size",
    "}",
    "var k = Known()",
    "io.print(k.size)",
    "",
  ].join("\n");
  send({
    jsonrpc: "2.0",
    method: "textDocument/didOpen",
    params: { textDocument: { uri: linkedUri, languageId: "little", version: 1, text: linkedSource } }
  });
  await waitFor((message) => message.method === "textDocument/publishDiagnostics" && message.params.uri === linkedUri, "linked diagnostics");

  send({ jsonrpc: "2.0", id: 35, method: "textDocument/hover", params: { textDocument: { uri: linkedUri }, position: { line: 2, character: 7 } } });
  const linkedClass = await waitFor((message) => message.id === 35, "linked class hover");
  assert.ok(linkedClass.result);
  const classValue = linkedClass.result.contents.value;
  assert.equal(classValue.split("class Known").length - 1, 1, "class name rendered twice");
  assert.match(classValue, /the size/);

  send({ jsonrpc: "2.0", id: 36, method: "textDocument/definition", params: { textDocument: { uri: linkedUri }, position: { line: 6, character: 12 } } });
  const linkedDefinition = await waitFor((message) => message.id === 36, "linked definition");
  assert.equal(linkedDefinition.result.range.start.line, 3);
  assert.equal(linkedDefinition.result.range.start.character, 2);

  send({ jsonrpc: "2.0", id: 37, method: "shutdown" });
  const finalShutdown = await waitFor((message) => message.id === 37, "shutdown");
  assert.equal(finalShutdown.result, null);

  send({ jsonrpc: "2.0", method: "exit" });
  const code = await closed;
  assert.equal(code, 0, "server exited with a failure code");
  assert.equal(stderr, "", "server wrote to stderr");
}

main().then(
  () => { console.log("lsp tests passed"); },
  (error) => {
    console.error(error);
    console.error(`server exited: ${server.exitCode}`);
    console.error(`stderr: ${stderr}`);
    server.kill();
    process.exit(1);
  }
);
