
"""Currency calculator REPL: a front end for the C currency engine (currency_engine.c).

Engine protocol (one reply line per command):
    SET name CODE<num> | SETN name <num> | EVAL <expr> | VARS
    RATE FROM TO <rate> | CONVERT name CODE | QUIT
"""

import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time

import requests

ENGINE_TIMEOUT = 3.0  # seconds to wait for one engine reply
RATE_TTL = 300        # seconds a fetched rate table is reused (per source currency)
TEMP_VAR_PREFIX = "__tmp"
RATE_API_URLS = (
    "https://cdn.jsdelivr.net/npm/@fawazahmed0/currency-api@latest/v1/currencies/{}.json",
    "https://latest.currency-api.pages.dev/v1/currencies/{}.json",  # fallback mirror
)

# Symbols / explicit prefixes -> ISO code. A bare '$' is USD; other dollars need a prefix.
SYMBOL_TO_CODE = {
    '₹': 'INR', '$': 'USD', '€': 'EUR', '£': 'GBP', '¥': 'CNY', '₩': 'KRW',
    '₽': 'RUB', '₺': 'TRY', 'R$': 'BRL',
    'US$': 'USD', 'CAD$': 'CAD', 'AUD$': 'AUD', 'SGD$': 'SGD', 'NZD$': 'NZD', 'MEX$': 'MXN',
}

# ISO code -> display symbol. Its keys are exactly the codes the C engine accepts.
CODE_TO_SYMBOL = {
    'INR': '₹', 'USD': '$', 'EUR': '€', 'GBP': '£',
    'CNY': '¥', 'JPY': '¥', 'KRW': '₩', 'RUB': '₽',
    'SAR': 'SAR', 'AED': 'AED', 'BRL': 'R$', 'ZAR': 'R',
    'CAD': 'C$', 'AUD': 'A$', 'CHF': 'CHF', 'SGD': 'S$',
    'MXN': 'Mex$', 'TRY': '₺', 'NZD': 'NZ$',
}

# ---------------------------------------------------------------- patterns
_NUM = r'(?:\d+(?:\.\d*)?|\.\d+)'
_NUM_EXP = rf'{_NUM}(?:[eE][+-]?\d+)?'      # allows 5e3, 1.5E-2, etc. (money literals only)
_SYMBOLS = '|'.join(re.escape(s) for s in sorted(SYMBOL_TO_CODE, key=len, reverse=True))
_CODES = '|'.join(CODE_TO_SYMBOL)

MONEY_PREFIX_RE = re.compile(
    rf'(?<![\w])({_SYMBOLS}|{_CODES})\s*([+-]?{_NUM_EXP})(?![\w])', re.I | re.A)   # $5, CAD$5, EUR 5
MONEY_SUFFIX_RE = re.compile(
    rf'(?<![\w.])([+-]?{_NUM_EXP})\s*({_CODES})(?![\w])', re.I | re.A)              # 5 EUR
PLAIN_NUM_RE = re.compile(rf'[+-]?{_NUM}(?:[eE][+-]?\d+)?')
CONVERT_RE = re.compile(rf'convert\s+([A-Za-z_]\w*)\s+to\s+({_SYMBOLS}|[A-Za-z]{{3}})', re.I | re.A)
ASSIGN_RE = re.compile(r'([A-Za-z_]\w*)\s*=\s*(.*)', re.A)
OK_VALUE_RE = re.compile(r'OK (\S+) ([A-Z]+)')       # "OK 12.5 USD" / "OK 7 NONE"
VAR_VALUE_RE = re.compile(r'([A-Z]{3})?(.+)')        # "USD12.5" / "7"


# ---------------------------------------------------------------- helpers
def resolve_currency(spec):
    """'$' / 'cad$' / 'eur' -> ISO code (not validated)."""
    key = spec.upper()
    return SYMBOL_TO_CODE.get(key, key)


def parse_money(text):
    """'$5', 'CAD$ 5', 'EUR5', '5 eur' -> ('USD', '5') etc.; None if not a money literal."""
    if m := MONEY_PREFIX_RE.fullmatch(text):
        spec, num = m.groups()
    elif m := MONEY_SUFFIX_RE.fullmatch(text):
        num, spec = m.groups()
    else:
        return None
    return resolve_currency(spec), num


def parse_ok_value(reply):
    """'OK 12.5 USD' -> ('12.5', 'USD'); 'OK 7 NONE' -> ('7', None); anything else -> None."""
    if m := OK_VALUE_RE.fullmatch(reply):
        val, code = m.groups()
        return val, (None if code == "NONE" else code)
    return None


def fmt_money(val, code=None):
    """Format a number with its currency symbol; plain numbers are returned unchanged."""
    if not code:
        return val
    sign, val = ("-", val[1:]) if val.startswith("-") else ("", val)
    sym = CODE_TO_SYMBOL.get(code, code)
    return f"{sign}{sym} {val}" if sym == code else f"{sign}{sym}{val}"


# ---------------------------------------------------------------- engine process
class EngineError(Exception):
    pass


class EngineClient:
    """Builds, launches and talks to the C engine over stdin/stdout."""

    def __init__(self, source, timeout=ENGINE_TIMEOUT):
        self.source = source
        self.timeout = timeout
        exe = "engine_test.exe" if os.name == "nt" else "engine_test"
        self.binary = os.path.join(os.path.dirname(os.path.abspath(source)), exe)
        self.proc = None
        self._lines = queue.Queue()
        self.build()
        self.start()

    # -- build ----------------------------------------------------------
    @staticmethod
    def find_compiler():
        candidates = ("cl", "gcc", "clang") if os.name == "nt" else ("clang", "gcc")
        return next((c for c in candidates if shutil.which(c)), None)

    def _up_to_date(self):
        if not os.path.exists(self.binary):
            return False
        return (not os.path.exists(self.source)
                or os.path.getmtime(self.binary) >= os.path.getmtime(self.source))

    def build(self):
        """Compile the engine, unless the binary is already newer than the source."""
        if self._up_to_date():
            return
        if not os.path.exists(self.source):
            raise EngineError(f"C source file '{self.source}' not found.")
        compiler = self.find_compiler()
        if not compiler:
            raise EngineError("No C compiler found (checked cl, gcc, clang). Please install one.")

        print(f"⚙️ Compiling C engine using '{compiler}'...")
        if compiler == "cl":
            cmd = ["cl", "/nologo", self.source, f"/Fe:{self.binary}"]
        else:
            cmd = [compiler, "-std=c11", "-Wall", "-Wextra", "-pedantic",
                   self.source, "-o", self.binary, "-lm"]
        try:
            result = subprocess.run(cmd, capture_output=True, text=True)
        except OSError as e:
            raise EngineError(f"Could not run the compiler: {e}")
        if result.returncode != 0:
            # MSVC reports errors on stdout, GCC/Clang on stderr.
            raise EngineError(f"Compilation failed:\n{result.stderr or result.stdout}")

    # -- process --------------------------------------------------------
    def start(self):
        try:
            self.proc = subprocess.Popen(
                [os.path.abspath(self.binary)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,  # an unread stderr pipe could fill up and block
                text=True, encoding="utf-8", bufsize=1,
            )
        except OSError as e:
            raise EngineError(f"Could not start engine '{self.binary}': {e}")
        # One reader thread per process; it feeds a queue so every wait can have a timeout.
        self._lines = queue.Queue()
        threading.Thread(target=self._pump, args=(self.proc, self._lines), daemon=True).start()

    @staticmethod
    def _pump(proc, lines):
        for line in proc.stdout:
            lines.put(line.rstrip("\r\n"))
        lines.put(None)  # EOF: engine exited

    def _kill(self):
        self.proc.kill()
        self.proc.wait()

    def send(self, command):
        """Send one command and return the engine's one-line reply."""
        if self.proc.poll() is not None:
            print("🔄 Engine is not running. Restarting (stored variables and rates were lost)...")
            self.start()
        try:
            self.proc.stdin.write(command + "\n")
            self.proc.stdin.flush()
            line = self._lines.get(timeout=self.timeout)
        except queue.Empty:
            # A late reply would desynchronise request/response pairs, so reset the engine.
            self._kill()
            raise EngineError("engine response timed out")
        except OSError as e:
            raise EngineError(f"communication failed: {e}")
        if line is None:
            raise EngineError("engine terminated unexpectedly")
        return line.strip()

    def close(self):
        if self.proc is None or self.proc.poll() is not None:
            return
        try:
            self.send("QUIT")
            self.proc.wait(timeout=2)
        except (EngineError, subprocess.TimeoutExpired):
            self._kill()


# ---------------------------------------------------------------- REPL
class CurrencyCalculatorREPL:
    def __init__(self, c_source_file=None):
        if c_source_file is None:
            c_source_file = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                         "currency_engine.c")
        self.engine = EngineClient(c_source_file)
        self.session = requests.Session()
        self._rate_cache = {}  # source code -> (fetched_at, {target_code: rate})

    def ask(self, command):
        """Send a command; engine failures come back as an 'ERR ...' reply line."""
        try:
            return self.engine.send(command)
        except EngineError as e:
            return f"ERR {e}"

    # -- input handling -------------------------------------------------
    def parse_and_execute(self, user_input):
        """Handle one input line. Returns False when the user asked to quit."""
        line = user_input.strip()
        if not line:
            return True

        word = line.lower()
        if word in ("quit", "exit"):
            return False
        if word == "vars":
            self.show_vars()
        elif m := CONVERT_RE.fullmatch(line):          # convert price1 to CAD
            self.convert(*m.groups())
        elif m := ASSIGN_RE.fullmatch(line):           # price1 = $500
            self.assign(*m.groups())
        else:                                          # arithmetic expression
            self.eval_expression(line)
        return True

    def assign(self, name, text):
        text = text.strip()
        if money := parse_money(text):
            code, num = money
        elif PLAIN_NUM_RE.fullmatch(text):
            code, num = None, text
        else:  # expression: evaluate first, then store the result
            processed = self.prepare_expression(text)
            if processed is None:
                return
            reply = self.ask(f"EVAL {processed}")
            if not (parsed := parse_ok_value(reply)):
                self.show_result(reply)
                return
            num, code = parsed
        cmd = f"SET {name} {code}{num}" if code else f"SETN {name} {num}"
        self.show_result(self.ask(cmd))
    
    def eval_expression(self, expr):
        processed = self.prepare_expression(expr)
        if processed is not None:
            self.show_result(self.ask(f"EVAL {processed}"))

    def convert(self, var_name, target_spec):
        target = resolve_currency(target_spec)
        if target not in CODE_TO_SYMBOL:
            print(f"❌ Error: Unsupported currency '{target_spec}'.")
            return

        # EVAL <var> returns the value and its currency (VARS output can be truncated).
        reply = self.ask(f"EVAL {var_name}")
        if not (parsed := parse_ok_value(reply)):
            self.show_result(reply)  # e.g. undefined variable
            return
        source = parsed[1]
        if not source:
            print(f"❌ Error: Variable '{var_name}' has no currency.")
            return
        if source == target:
            print(f"= Variable '{var_name}' is already in {target}.")
            return

        rate = self.get_rate(source, target)
        if rate is None:
            return
        rate_reply = self.ask(f"RATE {source} {target} {rate}")
        if not rate_reply.startswith("OK"):
            print(f"❌ Engine rejected rate: {rate_reply}")
            return
        self.show_result(self.ask(f"CONVERT {var_name} {target}"))

    def prepare_expression(self, expr):
        """Rewrite money literals in a raw expression as temp-variable references.

        'price1 + CAD250'  ->  'price1 + __tmp0'   (after SET __tmp0 CAD250)
        '5 EUR - 2'        ->  '__tmp0 - 2'        (after SET __tmp0 EUR5)

        Returns the rewritten expression, or None if a temp var could not be
        created (in which case the engine's error has already been printed).
        """
        counter = 0
        failed = []

        def make_temp(code, num):
            nonlocal counter
            name = f"{TEMP_VAR_PREFIX}{counter}"
            counter += 1
            reply = self.ask(f"SET {name} {code}{num}" if code else f"SETN {name} {num}")
            if not reply.startswith("OK"):
                failed.append(reply)
            return name

        def sub_prefix(m):
            spec, num = m.groups()
            return make_temp(resolve_currency(spec), num)

        def sub_suffix(m):
            num, spec = m.groups()
            return make_temp(resolve_currency(spec), num)

        # Prefix form first ($5, CAD$5, EUR 5), then suffix form (5 EUR) on what remains.
        rewritten = MONEY_PREFIX_RE.sub(sub_prefix, expr)
        if failed:
            self.show_result(failed[0])
            return None
        rewritten = MONEY_SUFFIX_RE.sub(sub_suffix, rewritten)
        if failed:
            self.show_result(failed[0])
            return None
        return rewritten
    # -- exchange rates -------------------------------------------------
    def get_rate(self, source, target):
        """Live rate source->target. One API call covers every target for that source."""
        cached = self._rate_cache.get(source)
        if cached and time.monotonic() - cached[0] < RATE_TTL:
            rates = cached[1]
        else:
            print(f"🌐 Fetching live exchange rates ({source})...")
            if (rates := self._fetch_rates(source)) is None:
                return None
            self._rate_cache[source] = (time.monotonic(), rates)

        rate = rates.get(target.lower())
        if rate is None:
            print(f"❌ Error: Exchange rate for {target} not found in API.")
        return rate

    def _fetch_rates(self, source):
        key, last_error = source.lower(), None
        for url in RATE_API_URLS:
            try:
                res = self.session.get(url.format(key), timeout=5)
                res.raise_for_status()
                return res.json()[key]
            except (requests.RequestException, ValueError, KeyError) as e:
                last_error = e
        print(f"⚠️ Network Error: Could not fetch live exchange rate ({last_error}). "
              "Skipping conversion.")
        return None

    # -- output ---------------------------------------------------------
    def show_result(self, reply):
        if parsed := parse_ok_value(reply):
            print(f"= {fmt_money(*parsed)}")
        elif reply.startswith("ERR"):
            print(f"❌ {reply}")
        else:  # "OK RATE", unexpected text, ...
            print(f"= {reply[3:] if reply.startswith('OK ') else reply}")

    def show_vars(self):
        reply = self.ask("VARS")
        if not reply.startswith("VARLIST"):
            self.show_result(reply)
            return
        items = [it for it in reply.split()[1:]
                 if not it.partition("=")[0].startswith(TEMP_VAR_PREFIX)]
        if not items:
            print("📦 Stored Variables: (None)")
            return
        print("📦 Stored Variables:")
        for item in items:
            name, _, raw = item.partition("=")
            code, val = VAR_VALUE_RE.fullmatch(raw).groups() if raw else (None, raw)
            print(f"  • {name} = {fmt_money(val, code)}")
    # -- main loop ------------------------------------------------------
    def run(self):
        print("\n==================================================")
        print("💱 Currency Toolkit REPL Initialized")
        print("ℹ️ Notice: The default symbol '$' points to US Dollars (USD).")
        print("   For other dollar currencies, use explicit prefixes like:")
        print("   CAD$ (Canadian), AUD$ (Australian), SGD$ (Singapore), NZD$ (New Zealand)")
        print("==================================================\n")

        try:
            while self.parse_and_execute(input("calc> ")):
                pass
        except (KeyboardInterrupt, EOFError):
            print("\nExiting...")
        finally:
            self.engine.close()


def main():
    try:
        repl = CurrencyCalculatorREPL()
    except EngineError as e:
        print(f"❌ {e}")
        sys.exit(1)
    repl.run()


if __name__ == "__main__":
    main()