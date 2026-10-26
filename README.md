here is my readme file...


# Currency Toolkit

A currency-aware expression calculator: a strict C11 engine for typed
money arithmetic, driven by a Python REPL that adds live exchange rates,
natural money-literal syntax, and process supervision.

## Architecture

```text
+-----------------------------------------------------+
|                    Python REPL                      |
|                 (currency_repl.py)                  |
|                                                     |
|  • Money-literal preprocessing                      |
|  • Live rate fetching                               |
|  • Result formatting                                |
|  • Process supervision                              |
+--------------------------+--------------------------+
                           |
             stdin/stdout  |  (line-based protocol)
                           |
+--------------------------v--------------------------+
|                    C11 Engine                       |
|                 (currency_engine.c)                 |
|                                                     |
|  • Lexer & Recursive-descent parser                 |
|  • Typed money arithmetic                           |
|  • Variable and exchange-rate tables                |
+-----------------------------------------------------+

## Requirements

### C engine
- A C11 compiler: `gcc`, `clang`, or MSVC (`cl`)
- Links against libm (`-lm` on Unix; automatic on Windows/MSVC)
- No third-party C libraries

### Python front end
- Python 3.8+
- `requests` (see `requirements.txt`)
- Network access to the currency API endpoints

## Install & Run

bash:
python -m pip install -r requirements.txt
python currency_repl.py
```
*(Note: The Python front end will automatically find your C compiler and compile `currency_engine.c` on the first run, or when the source file changes.)*

## Testing

The project has a unified test suite that covers the C engine internals, the Python REPL logic, and the full Python ↔ C integration.

```bash
# Install test dependencies
python -m pip install -r requirements_dev.txt

# Run all tests (Python + C)
pytest -v
```

To skip the slow integration tests (which compile and launch the C engine):

```bash
pytest -v -m "not slow"
```

## Engine Protocol

The Python front end and the C engine communicate over a line-oriented
stdin/stdout protocol: one command line in, exactly one reply line out.

| Command                  | Reply                                |
|--------------------------|--------------------------------------|
| `SET <name> <CODE><num>` | `OK <num> <CODE>`                    |
| `SETN <name> <num>`      | `OK <num> NONE`                      |
| `EVAL <expr>`            | `OK <num> <CODE-or-NONE>` / `ERR …`  |
| `VARS`                   | `VARLIST <name>=<CODE><num> …`       |
| `RATE <FROM> <TO> <r>`   | `OK RATE` / `ERR …`                  |
| `CONVERT <name> <CODE>`  | `OK <num> <CODE>` / `ERR …`          |
| `QUIT`                   | `OK BYE`                             |

### Why a pipe, not a socket, a shared library, or FFI?

- **Pipe (chosen):** zero dependencies, works identically on Linux/macOS/Windows,
  trivially debuggable (you can `printf 'EVAL 1+1\n' | ./engine_test`),
  and the protocol is the contract — either side can be replaced without
  touching the other. The cost is one process and a few microseconds of
  IPC per command; irrelevant for an interactive calculator.
- **Socket:** adds a port, a bind step, and a lifecycle to manage. Buys
  nothing for a local single-user tool.
- **Shared library / FFI:** would couple the Python and C build systems,
  make the C side harder to test in isolation, and turn every Python
  crash into a potential C crash. The protocol seam is the whole point.
```
