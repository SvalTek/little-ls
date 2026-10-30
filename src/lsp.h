#pragma once

/* Serves the language server protocol over stdin/stdout until the client
   sends the exit notification or closes the stream. */
void ltls_serve_stdio(void);
