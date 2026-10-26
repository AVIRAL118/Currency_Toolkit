"""pytest suite for currency_repl.py.

Covers:
  * pure helpers: parse_money, parse_ok_value, fmt_money
  * REPL methods with a mocked EngineClient: prepare_expression, assign
  * one real end-to-end integration test against currency_engine.c (marked slow)
"""

import os
import shutil
import sys
from unittest.mock import MagicMock, call
import subprocess
import pytest

# Make the module importable regardless of where pytest is invoked from.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import currency_repl
from currency_repl import (
    CurrencyCalculatorREPL,
    EngineClient,
    EngineError,
    fmt_money,
    parse_money,
    parse_ok_value,
)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
C_SOURCE = os.path.join(ROOT, "currency_engine.c")

# --------------------------------------------------------------------------- fixtures
def make_repl_with_mock_engine(send_side_effect=None, send_return_value="OK 0 NONE"):
    """Build a REPL without constructing a real EngineClient.

    `CurrencyCalculatorREPL.__init__` compiles and launches the C engine, which we
    do not want in unit tests. We bypass it and inject only the attributes that
    the methods under test touch.
    """
    repl = CurrencyCalculatorREPL.__new__(CurrencyCalculatorREPL)
    repl.engine = MagicMock(name="EngineClient")
    if send_side_effect is not None:
        repl.engine.send.side_effect = send_side_effect
    else:
        repl.engine.send.return_value = send_return_value
    repl.session = MagicMock(name="requests.Session")
    repl._rate_cache = {}
    return repl


# =========================================================================== 1
def test_parse_money_dollar_prefix():
    assert parse_money("$5") == ("USD", "5")


# =========================================================================== 2
def test_parse_money_suffix_form():
    assert parse_money("5 EUR") == ("EUR", "5")


# =========================================================================== 3
def test_parse_money_multi_char_prefix():
    assert parse_money("CAD$5") == ("CAD", "5")


# =========================================================================== 4
def test_parse_money_plain_number_is_not_money():
    assert parse_money("42") is None
    assert parse_money("hello") is None


# =========================================================================== 5
def test_prepare_expression_rewrites_bare_literal_and_issues_set():
    repl = make_repl_with_mock_engine(send_return_value="OK 250 CAD")

    out = repl.prepare_expression("CAD250")

    # The literal is replaced by a temp-variable name.
    assert out == f"{currency_repl.TEMP_VAR_PREFIX}0"

    # Exactly one SET command was issued, with the resolved currency code + number.
    repl.engine.send.assert_called_once_with(
        f"SET {currency_repl.TEMP_VAR_PREFIX}0 CAD250"
    )


# =========================================================================== 6
def test_prepare_expression_without_money_literals_is_unchanged():
    repl = make_repl_with_mock_engine()

    original = "price1 + price2"
    out = repl.prepare_expression(original)

    assert out == original
    repl.engine.send.assert_not_called()


# =========================================================================== 7
def test_prepare_expression_surfaces_engine_error_and_returns_none():
    # CAD is a known currency, so the regex matches and a SET is attempted.
    # The engine (mocked) rejects it, and the error must be surfaced.
    repl = make_repl_with_mock_engine(send_return_value="ERR engine rejected value")
    repl.show_result = MagicMock(name="show_result")

    out = repl.prepare_expression("CAD100")

    assert out is None
    repl.show_result.assert_called_once()
    surfaced = repl.show_result.call_args.args[0]
    assert surfaced.startswith("ERR")


# =========================================================================== 8
def test_assign_plain_money_literal_sends_correct_set():
    repl = make_repl_with_mock_engine(send_return_value="OK 5 USD")
    repl.show_result = MagicMock(name="show_result")

    repl.assign("x", "$5")

    repl.engine.send.assert_called_once_with("SET x USD5")
    # The OK reply is passed through to the display routine.
    repl.show_result.assert_called_once_with("OK 5 USD")


# =========================================================================== 9
def test_fmt_money_formats_symbol_before_value():
    assert "$" in fmt_money("5", "USD")


# =========================================================================== 10
def test_fmt_money_negative_keeps_sign_before_symbol():
    out = fmt_money("-5", "USD")
    # Sign must come first, then the symbol, then the digits.
    assert out.startswith("-$"), out
    assert out.endswith("5"), out


# =========================================================================== 11
def test_parse_ok_value_parses_currency_and_none():
    assert parse_ok_value("OK 12.5 USD") == ("12.5", "USD")
    assert parse_ok_value("OK 7 NONE") == ("7", None)


# =========================================================================== 12
def test_prepare_expression_unknown_code_passes_through():
    repl = make_repl_with_mock_engine()
    assert repl.prepare_expression("XYZ100") == "XYZ100"
    repl.engine.send.assert_not_called()

def test_c_engine_unit_tests():
    """Compiles and runs the C unit test suite."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    c_test_source = os.path.join(root, "tests", "test_currency_engine.c")
    binary = os.path.join(root, "engine_tests")

    # 1. Compile the C test
    compile_cmd = ["gcc", "-std=c11", "-Wall", "-Wextra", "-pedantic", 
                   "-I.", c_test_source, "-o", binary, "-lm"]
    compile_res = subprocess.run(compile_cmd, cwd=root, capture_output=True, text=True)
    assert compile_res.returncode == 0, f"Compilation failed:\n{compile_res.stderr}"

    # 2. Run the binary
    run_res = subprocess.run([binary], cwd=root, capture_output=True, text=True)
    assert run_res.returncode == 0, f"C tests failed:\n{run_res.stdout}\n{run_res.stderr}"
    assert "All unit tests passed successfully!" in run_res.stdout
#==============================================================================13
@pytest.mark.slow
@pytest.mark.skipif(
    EngineClient.find_compiler() is None,
    reason="no C compiler (cl/gcc/clang) available",
)
@pytest.mark.skipif(not os.path.exists(C_SOURCE), reason="currency_engine.c not found")
def test_end_to_end_real_engine():
    """Real Python <-> C round trip: SET then EVAL, no mocks at all."""
    engine = EngineClient(C_SOURCE)
    try:
        assert engine.send("SET x USD10").startswith("OK")
        reply = engine.send("EVAL x + 5")
        assert reply == "OK 15 USD"
    finally:
        engine.close()
