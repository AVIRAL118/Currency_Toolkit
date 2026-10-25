## Requirements

### C engine
- A C11 compiler: `gcc`, `clang`, or MSVC (`cl`)
- Links against libm (`-lm` on Unix; automatic on Windows/MSVC)
- No third-party C libraries

### Python front end
- Python 3.8+
- `requests` (see `requirements.txt`)
- Network access to the currency API endpoints

## Install & run

```bash
python -m pip install -r requirements.txt
python calculator.py
# Currency Toolkit

A currency-aware expression calculator: a strict C11 engine for typed
money arithmetic, driven by a Python REPL that adds live exchange rates,
natural money-literal syntax, and process supervision.

 Architecture Diagram
  ┌─────────────────────────┐         ┌──────────────────────────┐
│   Python REPL           │  stdin  │   C11 engine             │
│   (currency_repl.py)    │ ──────► │   (currency_engine.c)    │
│                         │         │                          │
│  • money-literal        │  stdout │  • lexer                 │
│    preprocessing        │ ◄────── │  • recursive-descent     │
│  • rate fetching        │         │    parser                │
│  • formatting           │         │  • typed arithmetic      │
│  • process supervision  │         │  • variable/rate tables  │
└─────────────────────────┘         └──────────────────────────┘

 Protocol_Spec_Section
 ## Engine protocol

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
