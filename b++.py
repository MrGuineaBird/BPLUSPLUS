import argparse
import ast
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


CURRENT_VERSION = "4.0"
INSTALL_APP_NAME = "Bpp"
WINDOWS_FILE_TYPE = "Bpp.Source"
UPDATE_CONFIG_NAME = "updates.json"
INSTALL_MANIFEST_NAME = "install.json"
DEFAULT_UPDATE_INTERVAL_HOURS = 24
UPDATE_REPO = "MrGuineaBird/BPLUSPLUS"
LINUX_INSTALL_DIR_NAME = "bpp"
BPP_CONSTANTS = {
    "true": True,
    "false": False,
    "nothing": None,
    "nil": None,
}


class CompileError(Exception):
    def __init__(self, message, lineno=None, line=None):
        self.message = message
        self.lineno = lineno
        self.line = line
        super().__init__(self.__str__())

    def __str__(self):
        if self.lineno is None:
            return self.message
        detail = f"[Line {self.lineno}] {self.message}"
        if self.line:
            detail += f"\n    --> {self.line}"
        return detail


def tokenize(src):
    tokens = []
    for lineno, line in enumerate(src.splitlines(), start=1):
        cleaned = _strip_comment(line)
        stripped = cleaned.strip()
        if stripped:
            tokens.append((lineno, stripped))
    return tokens


def _strip_comment(line):
    quote = None
    escaped = False
    result = []

    for ch in line:
        if escaped:
            result.append(ch)
            escaped = False
            continue

        if ch == "\\" and quote:
            result.append(ch)
            escaped = True
            continue

        if ch in {"'", '"'}:
            if quote is None:
                quote = ch
            elif quote == ch:
                quote = None
            result.append(ch)
            continue

        if ch == "#" and quote is None:
            break

        result.append(ch)

    return "".join(result)


def _split_args(arg_str):
    args = []
    cur = []
    stack = []
    quote = None
    escaped = False

    for ch in arg_str:
        if escaped:
            cur.append(ch)
            escaped = False
            continue

        if ch == "\\" and quote:
            cur.append(ch)
            escaped = True
            continue

        if ch in {"'", '"'}:
            if quote is None:
                quote = ch
            elif quote == ch:
                quote = None
            cur.append(ch)
            continue

        if quote:
            cur.append(ch)
            continue

        if ch in "([{":
            stack.append(ch)
            cur.append(ch)
            continue

        if ch in ")]}":
            if stack:
                opener = stack[-1]
                if (
                    (opener == "(" and ch == ")")
                    or (opener == "[" and ch == "]")
                    or (opener == "{" and ch == "}")
                ):
                    stack.pop()
            cur.append(ch)
            continue

        if ch == "," and not stack:
            args.append("".join(cur).strip())
            cur = []
            continue

        cur.append(ch)

    last = "".join(cur).strip()
    if last:
        args.append(last)
    return args


def _partition_keyword(text, keyword):
    stack = []
    quote = None
    escaped = False
    i = 0

    while i < len(text):
        ch = text[i]

        if escaped:
            escaped = False
            i += 1
            continue

        if ch == "\\" and quote:
            escaped = True
            i += 1
            continue

        if ch in {"'", '"'}:
            if quote is None:
                quote = ch
            elif quote == ch:
                quote = None
            i += 1
            continue

        if quote:
            i += 1
            continue

        if ch in "([{":
            stack.append(ch)
            i += 1
            continue

        if ch in ")]}":
            if stack:
                opener = stack[-1]
                if (
                    (opener == "(" and ch == ")")
                    or (opener == "[" and ch == "]")
                    or (opener == "{" and ch == "}")
                ):
                    stack.pop()
            i += 1
            continue

        if not stack and text.startswith(keyword, i):
            return text[:i].strip(), text[i + len(keyword) :].strip()

        i += 1

    return None, None


def _is_quoted(value):
    return (
        len(value) >= 2
        and ((value[0] == value[-1] == '"') or (value[0] == value[-1] == "'"))
    )


def _literal_string(value, lineno, line):
    try:
        parsed = ast.literal_eval(value)
    except Exception as exc:
        raise CompileError(f"Invalid string literal: {exc}", lineno, line) from exc

    if not isinstance(parsed, str):
        raise CompileError("Expected a string literal", lineno, line)
    return parsed


def _starts_keyword(line, keyword):
    return line == keyword or line.startswith(keyword + " ")


def _validate_identifier(name, what, lineno, line):
    if not name.isidentifier() or keyword_is_reserved(name):
        raise CompileError(f"Invalid {what}: {name}", lineno, line)


def keyword_is_reserved(name):
    return name in BPP_CONSTANTS or name in {
        "False",
        "None",
        "True",
        "and",
        "as",
        "assert",
        "async",
        "await",
        "break",
        "class",
        "continue",
        "def",
        "del",
        "elif",
        "else",
        "except",
        "finally",
        "for",
        "from",
        "global",
        "if",
        "import",
        "in",
        "is",
        "lambda",
        "nonlocal",
        "not",
        "or",
        "pass",
        "raise",
        "return",
        "try",
        "while",
        "with",
        "yield",
    }


class BppExpressionTransformer(ast.NodeTransformer):
    def visit_Name(self, node):
        if isinstance(node.ctx, ast.Load) and node.id in BPP_CONSTANTS:
            return ast.copy_location(ast.Constant(value=BPP_CONSTANTS[node.id]), node)
        return node

    def visit_BinOp(self, node):
        self.generic_visit(node)
        if isinstance(node.op, ast.Add):
            return ast.copy_location(
                ast.Call(
                    func=ast.Name(id="__bpp_add", ctx=ast.Load()),
                    args=[node.left, node.right],
                    keywords=[],
                ),
                node,
            )
        return node


def compile_expr(expr, lineno=None, line=None):
    expr = expr.strip()
    if not expr:
        raise CompileError("Empty expression", lineno, line)

    try:
        node = ast.parse(expr, mode="eval")
    except SyntaxError as exc:
        raise CompileError(f"Invalid expression: {expr}", lineno, line) from exc

    node = BppExpressionTransformer().visit(node)
    ast.fix_missing_locations(node)
    return ast.unparse(node.body)


def validate_assignment_target(target, lineno, line):
    target = target.strip()
    if not target:
        raise CompileError("Missing assignment target", lineno, line)

    try:
        parsed = ast.parse(target, mode="eval")
    except SyntaxError as exc:
        raise CompileError(f"Invalid assignment target: {target}", lineno, line) from exc

    allowed = (
        ast.Name,
        ast.Attribute,
        ast.Subscript,
        ast.Tuple,
        ast.List,
        ast.Starred,
    )

    for node in ast.walk(parsed.body):
        if isinstance(node, ast.Call):
            raise CompileError("Assignment target cannot call a function", lineno, line)
        if isinstance(node, ast.Name) and keyword_is_reserved(node.id):
            raise CompileError(f"Invalid assignment target: {target}", lineno, line)
        if isinstance(node, (ast.Name, ast.Attribute, ast.Subscript, ast.Tuple, ast.List, ast.Starred)):
            continue
        if isinstance(node, (ast.Load, ast.Store, ast.Constant, ast.Slice)):
            continue
        if not isinstance(node, allowed):
            raise CompileError(f"Invalid assignment target: {target}", lineno, line)

    return target


class Compiler:
    def __init__(self, base_dir=None):
        self.base_dir = os.path.abspath(base_dir or os.getcwd())
        self.temp_counter = 0
        self.module_counter = 0
        self.module_cache = {}
        self.module_blocks = []

    def compile(self, src, source_path=None):
        if source_path:
            self.base_dir = os.path.dirname(os.path.abspath(source_path))

        lines = tokenize(src)
        body, index = self.compile_block(
            lines,
            start=0,
            indent=0,
            function_depth=0,
            loop_depth=0,
            stop_at_end=False,
            stop_at_sibling=False,
        )
        if index != len(lines):
            lineno, line = lines[index]
            raise CompileError("Unexpected trailing statement", lineno, line)

        return self.render(body, source_path)

    def compile_block(
        self,
        lines,
        start,
        indent,
        function_depth,
        loop_depth,
        stop_at_end,
        stop_at_sibling,
    ):
        output = []
        i = start

        while i < len(lines):
            lineno, line = lines[i]

            if line == "end":
                if stop_at_end:
                    return output, i
                raise CompileError("Unexpected 'end'", lineno, line)

            if line.startswith("elif ") or line == "else:":
                if stop_at_sibling:
                    return output, i
                raise CompileError("Unexpected conditional branch", lineno, line)

            compiled, i = self.compile_statement(
                lines, i, indent, function_depth, loop_depth
            )
            output.extend(compiled)

        if stop_at_end:
            raise CompileError("Missing 'end'")

        return output, i

    def compile_statement(self, lines, index, indent, function_depth, loop_depth):
        lineno, line = lines[index]

        if line.startswith("def ") and line.endswith(":"):
            return self.compile_def(lines, index, indent, function_depth)

        if _starts_keyword(line, "return"):
            if function_depth == 0:
                raise CompileError("'return' outside function", lineno, line)
            rest = line[len("return") :].strip()
            if rest:
                return [self.emit(f"return {compile_expr(rest, lineno, line)}", indent)], index + 1
            return [self.emit("return None", indent)], index + 1

        if line.startswith("export "):
            name = line[len("export ") :].strip()
            if _is_quoted(name):
                name = repr(_literal_string(name, lineno, line))
            else:
                name = repr(name)
            return [self.emit(f"__bpp_export_name__ = {name}", indent)], index + 1

        if line.startswith("say "):
            say_src = line[4:].strip()
            expr_src, newline_mode = _partition_keyword(say_src, " without newline")
            if newline_mode == "":
                expr = compile_expr(expr_src, lineno, line)
                return [self.emit(f"print({expr}, end='')", indent)], index + 1
            expr = compile_expr(say_src, lineno, line)
            return [self.emit(f"print({expr})", indent)], index + 1

        if line.startswith("set "):
            target, expr = _partition_keyword(line[4:].strip(), " to ")
            if target is None:
                raise CompileError("Invalid set syntax. Use: set name to value", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            expr = compile_expr(expr, lineno, line)
            return [self.emit(f"{target} = {expr}", indent)], index + 1

        if line.startswith("add "):
            expr, target = _partition_keyword(line[4:].strip(), " to ")
            if target is None:
                raise CompileError("Invalid add syntax. Use: add value to name", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            expr = compile_expr(expr, lineno, line)
            return [self.emit(f"{target} = __bpp_add({target}, {expr})", indent)], index + 1

        if line.startswith("subtract "):
            expr, target = _partition_keyword(line[9:].strip(), " from ")
            if target is None:
                raise CompileError("Invalid subtract syntax. Use: subtract value from name", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            expr = compile_expr(expr, lineno, line)
            return [self.emit(f"{target} = {target} - ({expr})", indent)], index + 1

        if line.startswith("multiply "):
            target, expr = _partition_keyword(line[9:].strip(), " by ")
            if target is None:
                raise CompileError("Invalid multiply syntax. Use: multiply name by value", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            expr = compile_expr(expr, lineno, line)
            return [self.emit(f"{target} = {target} * ({expr})", indent)], index + 1

        if line.startswith("divide "):
            target, expr = _partition_keyword(line[7:].strip(), " by ")
            if target is None:
                raise CompileError("Invalid divide syntax. Use: divide name by value", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            expr = compile_expr(expr, lineno, line)
            return [self.emit(f"{target} = __bpp_divide({target}, {expr})", indent)], index + 1

        if line.startswith("put "):
            expr, target = _partition_keyword(line[4:].strip(), " in ")
            if target is None:
                raise CompileError("Invalid put syntax. Use: put value in list", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            expr = compile_expr(expr, lineno, line)
            return [self.emit(f"{target}.append({expr})", indent)], index + 1

        if line.startswith("remove "):
            expr, target = _partition_keyword(line[7:].strip(), " from ")
            if target is None:
                raise CompileError("Invalid remove syntax. Use: remove value from list", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            expr = compile_expr(expr, lineno, line)
            return [self.emit(f"{target}.remove({expr})", indent)], index + 1

        if line.startswith("empty "):
            target = validate_assignment_target(line[6:].strip(), lineno, line)
            return [self.emit(f"{target}.clear()", indent)], index + 1

        if line.startswith("ask "):
            prompt, target = _partition_keyword(line[4:].strip(), " into ")
            if target is None:
                raise CompileError("Invalid ask syntax. Use: ask prompt into name", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            prompt = compile_expr(prompt, lineno, line)
            return [self.emit(f"{target} = input(str({prompt}) + ' ').strip()", indent)], index + 1

        if line.startswith("write "):
            expr, filename = _partition_keyword(line[6:].strip(), " to ")
            if filename is None:
                raise CompileError("Invalid write syntax. Use: write value to filename", lineno, line)
            expr = compile_expr(expr, lineno, line)
            filename = compile_expr(filename, lineno, line)
            return [self.emit(f"__bpp_write({expr}, {filename})", indent)], index + 1

        if line.startswith("read "):
            filename, target = _partition_keyword(line[5:].strip(), " into ")
            if target is None:
                raise CompileError("Invalid read syntax. Use: read filename into name", lineno, line)
            target = validate_assignment_target(target, lineno, line)
            filename = compile_expr(filename, lineno, line)
            return [self.emit(f"{target} = __bpp_read({filename})", indent)], index + 1

        if line.startswith("import "):
            return self.compile_import(line, lineno, indent), index + 1

        if line.startswith("repeat ") and line.endswith(" times:"):
            return self.compile_repeat(lines, index, indent, function_depth, loop_depth)

        if line.startswith("repeat ") and line.endswith(":"):
            return self.compile_repeat(lines, index, indent, function_depth, loop_depth)

        if line.startswith("for each ") and line.endswith(":"):
            return self.compile_for_each(lines, index, indent, function_depth, loop_depth)

        if line == "forever:":
            return self.compile_forever(lines, index, indent, function_depth, loop_depth)

        if line.startswith("while ") and line.endswith(":"):
            return self.compile_while(lines, index, indent, function_depth, loop_depth)

        if line.startswith("if ") and line.endswith(":"):
            return self.compile_if(lines, index, indent, function_depth, loop_depth)

        if line == "stop loop":
            if loop_depth == 0:
                raise CompileError("'stop loop' can only be used inside a loop", lineno, line)
            return [self.emit("break", indent)], index + 1

        if line == "next loop":
            if loop_depth == 0:
                raise CompileError("'next loop' can only be used inside a loop", lineno, line)
            return [self.emit("continue", indent)], index + 1

        return self.compile_python_passthrough(line, lineno, indent), index + 1

    def compile_def(self, lines, index, indent, function_depth):
        lineno, line = lines[index]
        match = re.fullmatch(r"def\s+([A-Za-z_]\w*)\s*\((.*)\):", line)
        if not match:
            raise CompileError("Invalid function syntax. Use: def name(arg, arg):", lineno, line)

        name, params_src = match.groups()
        _validate_identifier(name, "function name", lineno, line)
        params = _split_args(params_src) if params_src.strip() else []
        for param in params:
            _validate_identifier(param, "parameter name", lineno, line)

        body, jump = self.compile_block(
            lines,
            start=index + 1,
            indent=indent + 1,
            function_depth=function_depth + 1,
            loop_depth=0,
            stop_at_end=True,
            stop_at_sibling=False,
        )

        if jump >= len(lines) or lines[jump][1] != "end":
            raise CompileError("Missing 'end' for function", lineno, line)

        compiled = [self.emit(f"def {name}({', '.join(params)}):", indent)]
        compiled.extend(body if body else [self.emit("pass", indent + 1)])
        return compiled, jump + 1

    def compile_repeat(self, lines, index, indent, function_depth, loop_depth):
        lineno, line = lines[index]
        match = re.fullmatch(r"repeat\s+(.+)\s+times(?:\s+as\s+([A-Za-z_]\w*))?:", line)
        if not match:
            raise CompileError("Invalid repeat syntax. Use: repeat count times:", lineno, line)

        count = compile_expr(match.group(1), lineno, line)
        var = match.group(2)
        if var:
            _validate_identifier(var, "repeat variable", lineno, line)
        else:
            var = self.next_temp("repeat")
        body, jump = self.compile_block(
            lines,
            start=index + 1,
            indent=indent + 1,
            function_depth=function_depth,
            loop_depth=loop_depth + 1,
            stop_at_end=True,
            stop_at_sibling=False,
        )

        if jump >= len(lines) or lines[jump][1] != "end":
            raise CompileError("Missing 'end' for repeat", lineno, line)

        if match.group(2):
            compiled = [self.emit(f"for {var} in range(1, int({count}) + 1):", indent)]
        else:
            compiled = [self.emit(f"for {var} in range(int({count})):", indent)]
        compiled.extend(body if body else [self.emit("pass", indent + 1)])
        return compiled, jump + 1

    def compile_for_each(self, lines, index, indent, function_depth, loop_depth):
        lineno, line = lines[index]
        match = re.fullmatch(r"for\s+each\s+([A-Za-z_]\w*)\s+in\s+(.+):", line)
        if not match:
            raise CompileError("Invalid for each syntax. Use: for each name in list:", lineno, line)

        name, values_src = match.groups()
        _validate_identifier(name, "loop variable", lineno, line)
        values = compile_expr(values_src, lineno, line)
        body, jump = self.compile_block(
            lines,
            start=index + 1,
            indent=indent + 1,
            function_depth=function_depth,
            loop_depth=loop_depth + 1,
            stop_at_end=True,
            stop_at_sibling=False,
        )

        if jump >= len(lines) or lines[jump][1] != "end":
            raise CompileError("Missing 'end' for for each", lineno, line)

        compiled = [self.emit(f"for {name} in {values}:", indent)]
        compiled.extend(body if body else [self.emit("pass", indent + 1)])
        return compiled, jump + 1

    def compile_forever(self, lines, index, indent, function_depth, loop_depth):
        lineno, line = lines[index]
        body, jump = self.compile_block(
            lines,
            start=index + 1,
            indent=indent + 1,
            function_depth=function_depth,
            loop_depth=loop_depth + 1,
            stop_at_end=True,
            stop_at_sibling=False,
        )

        if jump >= len(lines) or lines[jump][1] != "end":
            raise CompileError("Missing 'end' for forever", lineno, line)

        compiled = [self.emit("while True:", indent)]
        compiled.extend(body if body else [self.emit("pass", indent + 1)])
        return compiled, jump + 1

    def compile_while(self, lines, index, indent, function_depth, loop_depth):
        lineno, line = lines[index]
        cond = line[len("while ") : -1].strip()
        if not cond:
            raise CompileError("Missing while condition", lineno, line)

        body, jump = self.compile_block(
            lines,
            start=index + 1,
            indent=indent + 1,
            function_depth=function_depth,
            loop_depth=loop_depth + 1,
            stop_at_end=True,
            stop_at_sibling=False,
        )

        if jump >= len(lines) or lines[jump][1] != "end":
            raise CompileError("Missing 'end' for while", lineno, line)

        compiled = [self.emit(f"while {compile_expr(cond, lineno, line)}:", indent)]
        compiled.extend(body if body else [self.emit("pass", indent + 1)])
        return compiled, jump + 1

    def compile_if(self, lines, index, indent, function_depth, loop_depth):
        lineno, line = lines[index]
        cond = line[len("if ") : -1].strip()
        if not cond:
            raise CompileError("Missing if condition", lineno, line)

        body, jump = self.compile_block(
            lines,
            start=index + 1,
            indent=indent + 1,
            function_depth=function_depth,
            loop_depth=loop_depth,
            stop_at_end=True,
            stop_at_sibling=True,
        )

        compiled = [self.emit(f"if {compile_expr(cond, lineno, line)}:", indent)]
        compiled.extend(body if body else [self.emit("pass", indent + 1)])
        i = jump

        while i < len(lines):
            branch_lineno, branch_line = lines[i]

            if branch_line.startswith("elif ") and branch_line.endswith(":"):
                branch_cond = branch_line[len("elif ") : -1].strip()
                if not branch_cond:
                    raise CompileError("Missing elif condition", branch_lineno, branch_line)
                branch_body, i = self.compile_block(
                    lines,
                    start=i + 1,
                    indent=indent + 1,
                    function_depth=function_depth,
                    loop_depth=loop_depth,
                    stop_at_end=True,
                    stop_at_sibling=True,
                )
                compiled.append(self.emit(f"elif {compile_expr(branch_cond, branch_lineno, branch_line)}:", indent))
                compiled.extend(branch_body if branch_body else [self.emit("pass", indent + 1)])
                continue

            if branch_line == "else:":
                branch_body, i = self.compile_block(
                    lines,
                    start=i + 1,
                    indent=indent + 1,
                    function_depth=function_depth,
                    loop_depth=loop_depth,
                    stop_at_end=True,
                    stop_at_sibling=False,
                )
                compiled.append(self.emit("else:", indent))
                compiled.extend(branch_body if branch_body else [self.emit("pass", indent + 1)])
                break

            break

        if i >= len(lines) or lines[i][1] != "end":
            raise CompileError("Missing 'end' for if", lineno, line)

        return compiled, i + 1

    def compile_import(self, line, lineno, indent):
        arg = line[len("import ") :].strip()
        if not arg:
            raise CompileError("Missing import target", lineno, line)

        if _is_quoted(arg):
            module_ref = _literal_string(arg, lineno, line)
            function_name, export_name = self.compile_module(module_ref, lineno, line)
            _validate_identifier(export_name, "module export name", lineno, line)
            return [self.emit(f"{export_name} = __bpp_namespace({function_name}())", indent)]

        try:
            ast.parse(line, mode="exec")
        except SyntaxError as exc:
            raise CompileError(f"Invalid import syntax: {arg}", lineno, line) from exc
        return [self.emit(line, indent)]

    def compile_python_passthrough(self, line, lineno, indent):
        try:
            ast.parse(line, mode="exec")
        except SyntaxError as exc:
            raise CompileError(f"Unknown B++ statement: {line}", lineno, line) from exc
        return [self.emit(line, indent)]

    def compile_module(self, module_ref, lineno, line):
        module_path = self.resolve_module(module_ref, lineno, line)
        cached = self.module_cache.get(module_path)
        if cached:
            return cached

        self.module_counter += 1
        function_name = f"__bpp_module_{self.module_counter}_{self.safe_slug(os.path.splitext(os.path.basename(module_path))[0])}"
        src = self.read_text(module_path)
        lines = tokenize(src)
        export_name = self.find_export_name(lines, module_path)
        self.module_cache[module_path] = (function_name, export_name)

        old_base = self.base_dir
        self.base_dir = os.path.dirname(module_path)
        body, index = self.compile_block(
            lines,
            start=0,
            indent=1,
            function_depth=0,
            loop_depth=0,
            stop_at_end=False,
            stop_at_sibling=False,
        )
        self.base_dir = old_base

        if index != len(lines):
            mod_lineno, mod_line = lines[index]
            raise CompileError("Unexpected trailing module statement", mod_lineno, mod_line)

        block = [f"def {function_name}():"]
        block.extend(body if body else [self.emit("pass", 1)])
        block.append(self.emit("return __bpp_public_namespace(locals())", 1))
        self.module_blocks.append(block)
        return function_name, export_name

    def resolve_module(self, module_ref, lineno, line):
        filename = module_ref if module_ref.endswith(".bpm") else module_ref + ".bpm"
        candidates = [
            os.path.join(self.base_dir, "modules", filename),
            os.path.join(self.base_dir, filename),
        ]
        for candidate in candidates:
            if os.path.exists(candidate):
                return os.path.abspath(candidate)
        raise CompileError(f"Module '{module_ref}' not found", lineno, line)

    def find_export_name(self, lines, module_path):
        for _lineno, line in lines:
            if line.startswith("export "):
                raw = line[len("export ") :].strip()
                if _is_quoted(raw):
                    return _literal_string(raw, _lineno, line)
                return raw
        return os.path.splitext(os.path.basename(module_path))[0]

    def read_text(self, path):
        try:
            with open(path, "r", encoding="utf-8") as handle:
                return handle.read()
        except UnicodeDecodeError:
            with open(path, "r", encoding="utf-16") as handle:
                return handle.read()

    def render(self, body, source_path):
        lines = [
            "# Generated by the B++ compiler.",
            "# Source: " + (os.path.abspath(source_path) if source_path else "<stdin>"),
            "",
            "import types",
            "",
            "",
            "def __bpp_add(left, right):",
            "    if isinstance(left, str) or isinstance(right, str):",
            "        return str(left) + str(right)",
            "    return left + right",
            "",
            "",
            "def __bpp_divide(left, right):",
            "    if isinstance(right, (int, float)) and right == 0:",
            "        raise ZeroDivisionError('Division by zero')",
            "    result = left / right",
            "    if isinstance(result, float) and result.is_integer():",
            "        return int(result)",
            "    return result",
            "",
            "",
            "def __bpp_read(filename):",
            "    with open(str(filename), 'r', encoding='utf-8') as handle:",
            "        return handle.read()",
            "",
            "",
            "def __bpp_write(value, filename):",
            "    with open(str(filename), 'w', encoding='utf-8') as handle:",
            "        handle.write(str(value))",
            "",
            "",
            "def __bpp_public_namespace(values):",
            "    return {",
            "        name: value",
            "        for name, value in values.items()",
            "        if not name.startswith('__') and name != 'types'",
            "    }",
            "",
            "",
            "def __bpp_namespace(values):",
            "    return types.SimpleNamespace(**values)",
            "",
        ]

        if self.module_blocks:
            for block in self.module_blocks:
                lines.extend(block)
                lines.append("")

        lines.extend(body)
        if body:
            lines.append("")

        return "\n".join(lines)

    def emit(self, line, indent):
        return "    " * indent + line

    def next_temp(self, label):
        self.temp_counter += 1
        return f"__bpp_{label}_{self.temp_counter}"

    def safe_slug(self, value):
        slug = re.sub(r"\W+", "_", value).strip("_")
        if not slug:
            return "module"
        if slug[0].isdigit():
            slug = "_" + slug
        return slug


def compile_source(src, source_path=None):
    compiler = Compiler(base_dir=os.path.dirname(os.path.abspath(source_path)) if source_path else os.getcwd())
    return compiler.compile(src, source_path=source_path)


def default_output_path(source_path):
    root, _ext = os.path.splitext(os.path.abspath(source_path))
    return root + ".py"


def read_source(path):
    if path == "-":
        return sys.stdin.read()
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def write_output(path, compiled):
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(compiled)


def run_python(path, args):
    old_argv = sys.argv[:]
    namespace = {
        "__name__": "__main__",
        "__file__": os.path.abspath(path),
        "__package__": None,
    }
    try:
        sys.argv = [path] + list(args)
        with open(path, "r", encoding="utf-8") as handle:
            source = handle.read()
        exec(compile(source, path, "exec"), namespace)
    except SystemExit as exc:
        code = exc.code
        if code is None:
            return 0
        if isinstance(code, int):
            return code
        print(code, file=sys.stderr)
        return 1
    finally:
        sys.argv = old_argv
    return 0


def get_install_paths():
    if os.name != "nt":
        home = os.path.expanduser("~")
        data_home = os.environ.get("XDG_DATA_HOME") or os.path.join(home, ".local", "share")
        root = os.path.join(data_home, LINUX_INSTALL_DIR_NAME)
        bin_dir = os.environ.get("BPP_BIN_DIR") or os.path.join(home, ".local", "bin")
        return {
            "root": root,
            "bin": bin_dir,
            "compiler": os.path.join(root, "bpp.py"),
            "bpp_exe": os.path.join(bin_dir, "bpp"),
            "bplusplus_exe": os.path.join(bin_dir, "b++"),
            "bpp_cmd": os.path.join(bin_dir, "bpp"),
            "bplusplus_cmd": os.path.join(bin_dir, "b++"),
            "bpp_run": os.path.join(bin_dir, "bpp-run"),
        }

    local_appdata = os.environ.get("LOCALAPPDATA")
    if not local_appdata:
        local_appdata = os.path.join(os.path.expanduser("~"), "AppData", "Local")

    root = os.path.join(local_appdata, INSTALL_APP_NAME)
    bin_dir = os.path.join(root, "bin")
    compiler_path = os.path.join(root, "bpp.py")
    return {
        "root": root,
        "bin": bin_dir,
        "compiler": compiler_path,
        "bpp_exe": os.path.join(bin_dir, "bpp.exe"),
        "bplusplus_exe": os.path.join(bin_dir, "b++.exe"),
        "bpp_cmd": os.path.join(bin_dir, "bpp.cmd"),
        "bplusplus_cmd": os.path.join(bin_dir, "b++.cmd"),
        "bpp_run": os.path.join(bin_dir, "bpp-run.cmd"),
    }


def get_update_config_path():
    return os.path.join(get_install_paths()["root"], UPDATE_CONFIG_NAME)


def get_install_manifest_path():
    return os.path.join(get_install_paths()["root"], INSTALL_MANIFEST_NAME)


def default_update_config():
    return {
        "auto_update": False,
        "interval_hours": DEFAULT_UPDATE_INTERVAL_HOURS,
        "last_checked": 0,
    }


def load_update_config():
    config = default_update_config()
    path = get_update_config_path()
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as handle:
                loaded = json.load(handle)
            if isinstance(loaded, dict):
                for key in config:
                    if key in loaded:
                        config[key] = loaded[key]
        except (OSError, json.JSONDecodeError):
            pass
    return config


def save_update_config(config):
    path = get_update_config_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(config, handle, indent=2, sort_keys=True)
        handle.write("\n")


def configure_updates(auto_update=None, interval_hours=None):
    config = load_update_config()
    if auto_update is not None:
        config["auto_update"] = bool(auto_update)
    if interval_hours is not None:
        if interval_hours <= 0:
            raise RuntimeError("Update interval must be greater than 0 hours.")
        config["interval_hours"] = interval_hours
    save_update_config(config)
    return config


def format_update_source(config):
    return "GitHub Releases: " + UPDATE_REPO


def http_get_json(url, timeout=20):
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/json",
            "User-Agent": f"Bpp/{CURRENT_VERSION}",
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def download_url(url, path, timeout=60):
    request = urllib.request.Request(url, headers={"User-Agent": f"Bpp/{CURRENT_VERSION}"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        with open(path, "wb") as handle:
            shutil.copyfileobj(response, handle)


def parse_version(value):
    value = str(value).strip()
    if value.startswith(("v", "V")):
        value = value[1:]
    numbers = re.findall(r"\d+", value)
    if not numbers:
        return None
    return tuple(int(part) for part in numbers)


def is_newer_version(remote, current):
    remote_key = parse_version(remote)
    current_key = parse_version(current)
    if remote_key is not None and current_key is not None:
        return remote_key > current_key
    return str(remote).strip() != str(current).strip()


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def preferred_update_asset_names():
    if os.name == "nt":
        return ["bpp.exe"]
    return ["bpp-linux", "bpp-linux-x64", "bpp"]


def pick_release_asset(assets, preferred_names=None):
    names = [name.lower() for name in (preferred_names or preferred_update_asset_names())]
    for preferred_name in names:
        for asset in assets:
            if asset.get("name", "").lower() == preferred_name:
                return asset
    for asset in assets:
        name = asset.get("name", "").lower()
        if os.name == "nt":
            if name.endswith(".exe") and "setup" not in name:
                return asset
        elif name.startswith("bpp") and ("linux" in name or name == "bpp"):
            return asset
    return None


def read_update_from_github(repo):
    release = http_get_json(f"https://api.github.com/repos/{repo}/releases/latest")
    asset = pick_release_asset(release.get("assets", []))
    if not asset:
        expected = ", ".join(preferred_update_asset_names())
        raise RuntimeError(f"Latest GitHub release does not have a B++ asset for this platform ({expected}).")

    version = release.get("tag_name") or release.get("name")
    if not version:
        raise RuntimeError("Latest GitHub release does not have a version tag.")

    return {
        "version": str(version).lstrip("vV"),
        "url": asset.get("browser_download_url"),
        "sha256": None,
        "source": f"https://github.com/{repo}/releases/latest",
    }


def find_remote_update(config=None):
    info = read_update_from_github(UPDATE_REPO)
    info["is_newer"] = is_newer_version(info["version"], CURRENT_VERSION)
    return info


def verify_download(path, expected_sha256):
    if not expected_sha256:
        return
    actual = file_sha256(path).lower()
    expected = expected_sha256.lower().strip()
    if actual != expected:
        raise RuntimeError(f"Downloaded update checksum mismatch: expected {expected}, got {actual}")


def write_deferred_update_script(download_path, targets, version):
    script_path = os.path.join(tempfile.gettempdir(), "bpp_apply_update.cmd")
    lines = [
        "@echo off",
        "setlocal",
        f'set "SRC={download_path}"',
    ]
    for index, target in enumerate(targets, start=1):
        lines.append(f'set "TARGET{index}={target}"')
    lines.extend(
        [
            ":wait",
            'copy /Y "%SRC%" "%TARGET1%" >nul 2>nul',
            "if errorlevel 1 (",
            "  timeout /t 1 /nobreak >nul",
            "  goto wait",
            ")",
        ]
    )
    for index in range(2, len(targets) + 1):
        lines.append(f'copy /Y "%SRC%" "%TARGET{index}%" >nul 2>nul')
    lines.extend(
        [
            'del "%SRC%" >nul 2>nul',
            f'echo B++ updated to {version}.',
            'del "%~f0" >nul 2>nul',
            "",
        ]
    )
    with open(script_path, "w", encoding="utf-8", newline="\r\n") as handle:
        handle.write("\n".join(lines))
    return script_path


def install_downloaded_update(download_path, version):
    paths = get_install_paths()
    targets = [paths["bpp_exe"], paths["bplusplus_exe"]]
    for target in targets:
        os.makedirs(os.path.dirname(target), exist_ok=True)

    if os.name != "nt":
        for target in targets:
            temp_target = target + ".new"
            shutil.copy2(download_path, temp_target)
            chmod_executable(temp_target)
            os.replace(temp_target, target)
        write_sh_run_launcher(paths["bpp_run"], paths["bpp_exe"])
        write_install_manifest(paths, "binary")
        try:
            os.remove(download_path)
        except OSError:
            pass
        return "installed"

    try:
        for target in targets:
            shutil.copy2(download_path, target)
    except PermissionError:
        script = write_deferred_update_script(download_path, targets, version)
        subprocess.Popen(
            ["cmd", "/c", script],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=0x00000008 | 0x00000200,
        )
        return "scheduled"

    try:
        os.remove(download_path)
    except OSError:
        pass
    return "installed"


def update_bpp(force=False, quiet=False):
    info = find_remote_update()
    if not info["is_newer"] and not force:
        if not quiet:
            print(f"B++ is already current ({CURRENT_VERSION}).")
        return False

    suffix = ".exe" if os.name == "nt" else ""
    fd, download_path = tempfile.mkstemp(prefix="bpp-update-", suffix=suffix)
    os.close(fd)
    try:
        if not quiet:
            print(f"Downloading B++ {info['version']} from {info['source']}")
        download_url(info["url"], download_path)
        verify_download(download_path, info.get("sha256"))
        result = install_downloaded_update(download_path, info["version"])
    except Exception:
        try:
            os.remove(download_path)
        except OSError:
            pass
        raise

    if not quiet:
        if result == "scheduled":
            print(f"B++ {info['version']} will finish installing after this process exits.")
        else:
            print(f"B++ updated to {info['version']}.")
    return True


def check_update_status():
    config = load_update_config()
    print("B++ updates")
    print(f"source: {format_update_source(config)}")
    print(f"auto_update: {'on' if config.get('auto_update') else 'off'}")
    print(f"interval_hours: {config.get('interval_hours', DEFAULT_UPDATE_INTERVAL_HOURS)}")
    if config.get("last_checked"):
        checked = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(config["last_checked"]))
        print(f"last_checked: {checked}")
    else:
        print("last_checked: never")


def check_for_update_command():
    info = find_remote_update()
    if info["is_newer"]:
        print(f"B++ {info['version']} is available from {info['source']}.")
        print("Run: bpp update")
    else:
        print(f"B++ is already current ({CURRENT_VERSION}).")


def auto_update_if_due(quiet=False):
    config = load_update_config()
    if not config.get("auto_update"):
        return

    interval = float(config.get("interval_hours", DEFAULT_UPDATE_INTERVAL_HOURS)) * 3600
    now = time.time()
    if now - float(config.get("last_checked", 0)) < interval:
        return

    config["last_checked"] = now
    save_update_config(config)
    try:
        update_bpp(quiet=quiet)
    except (RuntimeError, OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
        if not quiet:
            print(f"Update check failed: {exc}", file=sys.stderr)


def require_windows():
    if os.name != "nt":
        raise RuntimeError("This B++ feature currently supports Windows only.")


def current_compiler_path():
    path = os.path.abspath(sys.executable if getattr(sys, "frozen", False) else __file__)
    if not os.path.exists(path):
        raise RuntimeError("Cannot locate the current compiler file.")
    return path


def same_path(left, right):
    return os.path.normcase(os.path.abspath(left)) == os.path.normcase(os.path.abspath(right))


def write_cmd_launcher(path, python_exe, compiler_path):
    command = f'"{compiler_path}" %*' if compiler_path.lower().endswith(".exe") else f'"{python_exe}" "{compiler_path}" %*'
    content = "\n".join(
        [
            "@echo off",
            command,
            "",
        ]
    )
    with open(path, "w", encoding="utf-8", newline="\r\n") as handle:
        handle.write(content)


def shell_quote(value):
    return "'" + str(value).replace("'", "'\"'\"'") + "'"


def chmod_executable(path):
    try:
        mode = os.stat(path).st_mode
        os.chmod(path, mode | 0o755)
    except OSError:
        pass


def write_sh_launcher(path, python_exe, compiler_path):
    content = "\n".join(
        [
            "#!/bin/sh",
            "# Generated by B++ installer.",
            f"exec {shell_quote(python_exe)} {shell_quote(compiler_path)} \"$@\"",
            "",
        ]
    )
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)
    chmod_executable(path)


def write_sh_run_launcher(path, bpp_command_path):
    content = "\n".join(
        [
            "#!/bin/sh",
            "# Generated by B++ installer.",
            'DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)',
            'exec "$DIR/' + os.path.basename(bpp_command_path) + '" --quiet --run "$@"',
            "",
        ]
    )
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)
    chmod_executable(path)


def write_install_manifest(paths, mode):
    os.makedirs(paths["root"], exist_ok=True)
    manifest = {
        "version": CURRENT_VERSION,
        "mode": mode,
        "commands": [paths["bpp_exe"], paths["bplusplus_exe"], paths["bpp_run"]],
    }
    with open(get_install_manifest_path(), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")


def read_install_manifest():
    path = get_install_manifest_path()
    try:
        with open(path, "r", encoding="utf-8") as handle:
            manifest = json.load(handle)
        return manifest if isinstance(manifest, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def normalize_windows_path(path):
    return os.path.normcase(os.path.normpath(os.path.expandvars(path))).rstrip("\\/")


def split_env_list(value):
    return [part for part in value.split(os.pathsep) if part]


def add_user_env_entry(name, entry):
    import winreg

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, "Environment") as key:
        try:
            value, reg_type = winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            value = os.environ.get(name, "")
            reg_type = winreg.REG_EXPAND_SZ if "%" in value else winreg.REG_SZ

        parts = split_env_list(value)
        if name.upper() == "PATHEXT":
            exists = any(part.upper() == entry.upper() for part in parts)
        else:
            exists = any(normalize_windows_path(part) == normalize_windows_path(entry) for part in parts)

        if not exists:
            parts.append(entry)
            new_value = os.pathsep.join(parts)
            winreg.SetValueEx(key, name, 0, reg_type, new_value)
            os.environ[name] = new_value
            return True

    return False


def remove_user_env_entry(name, entry):
    import winreg

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, "Environment") as key:
        try:
            value, reg_type = winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            return False

        parts = split_env_list(value)
        if name.upper() == "PATHEXT":
            kept = [part for part in parts if part.upper() != entry.upper()]
        else:
            kept = [
                part
                for part in parts
                if normalize_windows_path(part) != normalize_windows_path(entry)
            ]

        if kept != parts:
            new_value = os.pathsep.join(kept)
            winreg.SetValueEx(key, name, 0, reg_type, new_value)
            os.environ[name] = new_value
            return True

    return False


def broadcast_environment_change():
    try:
        import ctypes

        hwnd_broadcast = 0xFFFF
        wm_settingchange = 0x001A
        smto_abortifhung = 0x0002
        ctypes.windll.user32.SendMessageTimeoutW(
            hwnd_broadcast,
            wm_settingchange,
            0,
            "Environment",
            smto_abortifhung,
            5000,
            None,
        )
    except Exception:
        pass


def set_default_registry_value(key, value):
    import winreg

    winreg.SetValueEx(key, "", 0, winreg.REG_SZ, value)


def register_windows_filetype(compiler_path):
    import winreg

    base = r"Software\Classes"
    if compiler_path.lower().endswith(".exe"):
        run_command = f'"{compiler_path}" --quiet --run "%1" -- %*'
        compile_command = f'"{compiler_path}" "%1"'
    else:
        python_exe = sys.executable
        run_command = f'"{python_exe}" "{compiler_path}" --quiet --run "%1" -- %*'
        compile_command = f'"{python_exe}" "{compiler_path}" "%1"'

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + r"\.bpp") as key:
        set_default_registry_value(key, WINDOWS_FILE_TYPE)

    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + "\\" + WINDOWS_FILE_TYPE) as key:
        set_default_registry_value(key, "B++ Source File")

    with winreg.CreateKey(
        winreg.HKEY_CURRENT_USER,
        base + "\\" + WINDOWS_FILE_TYPE + r"\shell\open\command",
    ) as key:
        set_default_registry_value(key, run_command)

    with winreg.CreateKey(
        winreg.HKEY_CURRENT_USER,
        base + "\\" + WINDOWS_FILE_TYPE + r"\shell\compile\command",
    ) as key:
        set_default_registry_value(key, compile_command)

    with winreg.CreateKey(
        winreg.HKEY_CURRENT_USER,
        base + "\\" + WINDOWS_FILE_TYPE + r"\shell\edit\command",
    ) as key:
        set_default_registry_value(key, 'notepad.exe "%1"')


def delete_registry_tree(root, path):
    import winreg

    try:
        with winreg.OpenKey(root, path, 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            while True:
                try:
                    child = winreg.EnumKey(key, 0)
                except OSError:
                    break
                delete_registry_tree(root, path + "\\" + child)
        winreg.DeleteKey(root, path)
        return True
    except FileNotFoundError:
        return False


def unregister_windows_filetype():
    import winreg

    base = r"Software\Classes"
    removed = delete_registry_tree(winreg.HKEY_CURRENT_USER, base + "\\" + WINDOWS_FILE_TYPE)

    extension_path = base + r"\.bpp"
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, extension_path, 0, winreg.KEY_READ) as key:
            value, _type = winreg.QueryValueEx(key, "")
        if value == WINDOWS_FILE_TYPE:
            removed = delete_registry_tree(winreg.HKEY_CURRENT_USER, extension_path) or removed
    except FileNotFoundError:
        pass

    return removed


def install_windows_bpp():
    paths = get_install_paths()
    try:
        os.makedirs(paths["bin"], exist_ok=True)
    except PermissionError:
        if not os.path.isdir(paths["bin"]):
            raise

    source_path = current_compiler_path()
    if getattr(sys, "frozen", False):
        for target in (paths["bpp_exe"], paths["bplusplus_exe"]):
            if normalize_windows_path(source_path) != normalize_windows_path(target):
                shutil.copy2(source_path, target)
        runner_path = paths["bpp_exe"]
    else:
        runner_path = paths["compiler"]
    if not getattr(sys, "frozen", False) and normalize_windows_path(source_path) != normalize_windows_path(paths["compiler"]):
        try:
            shutil.copy2(source_path, paths["compiler"])
        except PermissionError:
            if not os.path.exists(paths["compiler"]):
                raise
            print(f"Using existing compiler: {paths['compiler']}")

    for launcher in (paths["bpp_cmd"], paths["bplusplus_cmd"]):
        try:
            write_cmd_launcher(launcher, sys.executable, runner_path)
        except PermissionError:
            if not os.path.exists(launcher):
                raise
            print(f"Using existing command launcher: {launcher}")

    changed_path = add_user_env_entry("PATH", paths["bin"])
    changed_pathext = add_user_env_entry("PATHEXT", ".BPP")
    register_windows_filetype(runner_path)

    if changed_path or changed_pathext:
        broadcast_environment_change()

    print(f"Installed B++ compiler {CURRENT_VERSION}")
    print(f"Compiler: {paths['compiler']}")
    print(f"Commands: {paths['bpp_cmd']} and {paths['bplusplus_cmd']}")
    print(".bpp files now compile to .py and run through B++.")
    if changed_path or changed_pathext:
        print("Open a new terminal so PATH/PATHEXT changes are picked up.")


def install_linux_bpp():
    paths = get_install_paths()
    os.makedirs(paths["root"], exist_ok=True)
    os.makedirs(paths["bin"], exist_ok=True)

    source_path = current_compiler_path()
    if getattr(sys, "frozen", False):
        for target in (paths["bpp_exe"], paths["bplusplus_exe"]):
            if not same_path(source_path, target):
                shutil.copy2(source_path, target)
            chmod_executable(target)
        mode = "binary"
    else:
        if not same_path(source_path, paths["compiler"]):
            shutil.copy2(source_path, paths["compiler"])
        for launcher in (paths["bpp_exe"], paths["bplusplus_exe"]):
            write_sh_launcher(launcher, sys.executable, paths["compiler"])
        mode = "source"

    write_sh_run_launcher(paths["bpp_run"], paths["bpp_exe"])
    write_install_manifest(paths, mode)

    print(f"Installed B++ compiler {CURRENT_VERSION}")
    print(f"Compiler: {paths['compiler'] if mode == 'source' else paths['bpp_exe']}")
    print(f"Commands: {paths['bpp_exe']}, {paths['bplusplus_exe']}, and {paths['bpp_run']}")
    print("Use bpp --run file.bpp, or add '#!/usr/bin/env bpp-run' to executable .bpp scripts.")
    if paths["bin"] not in split_env_list(os.environ.get("PATH", "")):
        print(f"Add this to your PATH if needed: {paths['bin']}")


def install_bpp():
    if os.name == "nt":
        install_windows_bpp()
    else:
        install_linux_bpp()


def uninstall_windows_bpp():
    paths = get_install_paths()
    changed_path = remove_user_env_entry("PATH", paths["bin"])
    changed_pathext = remove_user_env_entry("PATHEXT", ".BPP")
    removed_registry = unregister_windows_filetype()

    if os.path.exists(paths["root"]):
        shutil.rmtree(paths["root"])

    if changed_path or changed_pathext or removed_registry:
        broadcast_environment_change()

    print("Uninstalled B++.")


def uninstall_linux_bpp():
    paths = get_install_paths()
    manifest = read_install_manifest()
    commands = manifest.get("commands")
    if not isinstance(commands, list) and not os.path.exists(paths["root"]):
        print("B++ is not installed for this user.")
        return
    if not isinstance(commands, list):
        commands = [paths["bpp_exe"], paths["bplusplus_exe"], paths["bpp_run"]]

    for command in commands:
        if isinstance(command, str) and os.path.exists(command):
            try:
                os.remove(command)
            except OSError:
                pass

    if os.path.exists(paths["root"]):
        shutil.rmtree(paths["root"])

    print("Uninstalled B++.")


def uninstall_bpp():
    if os.name == "nt":
        uninstall_windows_bpp()
    else:
        uninstall_linux_bpp()


def query_default_registry_value(path):
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ) as key:
            value, _type = winreg.QueryValueEx(key, "")
            return value
    except FileNotFoundError:
        return None


def get_user_or_process_env(name):
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment", 0, winreg.KEY_READ) as key:
            value, _type = winreg.QueryValueEx(key, name)
            return value
    except FileNotFoundError:
        return os.environ.get(name, "")


def env_contains(name, entry):
    value = get_user_or_process_env(name)
    parts = split_env_list(value)
    if name.upper() == "PATHEXT":
        return any(part.upper() == entry.upper() for part in parts)
    return any(normalize_windows_path(part) == normalize_windows_path(entry) for part in parts)


def doctor_windows_bpp():
    paths = get_install_paths()
    extension_value = query_default_registry_value(r"Software\Classes\.bpp")
    open_command = query_default_registry_value(
        r"Software\Classes" + "\\" + WINDOWS_FILE_TYPE + r"\shell\open\command"
    )

    checks = [
        ("compiler exe", os.path.exists(paths["bpp_exe"]), paths["bpp_exe"]),
        ("b++ exe", os.path.exists(paths["bplusplus_exe"]), paths["bplusplus_exe"]),
        ("legacy compiler", os.path.exists(paths["compiler"]), paths["compiler"]),
        ("legacy bpp command", os.path.exists(paths["bpp_cmd"]), paths["bpp_cmd"]),
        ("legacy b++ command", os.path.exists(paths["bplusplus_cmd"]), paths["bplusplus_cmd"]),
        ("PATH", env_contains("PATH", paths["bin"]), paths["bin"]),
        ("PATHEXT", env_contains("PATHEXT", ".BPP"), ".BPP"),
        (".bpp association", extension_value == WINDOWS_FILE_TYPE, extension_value or "missing"),
        (
            "open command",
            bool(
                open_command
                and (
                    paths["bpp_exe"] in open_command
                    or paths["compiler"] in open_command
                )
            ),
            open_command or "missing",
        ),
    ]

    for name, ok, detail in checks:
        status = "ok" if ok else "missing"
        print(f"{name}: {status} ({detail})")

    config = load_update_config()
    print(f"update source: {format_update_source(config)}")
    print(f"auto updates: {'on' if config.get('auto_update') else 'off'}")


def path_contains(entry):
    return any(os.path.abspath(part) == os.path.abspath(entry) for part in split_env_list(os.environ.get("PATH", "")))


def doctor_linux_bpp():
    paths = get_install_paths()
    compiler_ok = os.path.exists(paths["compiler"]) or (
        os.path.exists(paths["bpp_exe"]) and os.access(paths["bpp_exe"], os.X_OK)
    )
    checks = [
        ("installed compiler", compiler_ok, paths["compiler"]),
        ("bpp command", os.path.exists(paths["bpp_exe"]) and os.access(paths["bpp_exe"], os.X_OK), paths["bpp_exe"]),
        ("b++ command", os.path.exists(paths["bplusplus_exe"]) and os.access(paths["bplusplus_exe"], os.X_OK), paths["bplusplus_exe"]),
        ("bpp-run command", os.path.exists(paths["bpp_run"]) and os.access(paths["bpp_run"], os.X_OK), paths["bpp_run"]),
        ("PATH", path_contains(paths["bin"]), paths["bin"]),
    ]

    for name, ok, detail in checks:
        status = "ok" if ok else "missing"
        print(f"{name}: {status} ({detail})")

    config = load_update_config()
    print(f"update source: {format_update_source(config)}")
    print(f"auto updates: {'on' if config.get('auto_update') else 'off'}")


def doctor_bpp():
    if os.name == "nt":
        doctor_windows_bpp()
    else:
        doctor_linux_bpp()


def run_self_test():
    sample = """
def square(x):
    return x * x
end

set total to 0
repeat 3 times as turn:
    add turn to total
end

set names to []
put "A" in names
put "B" in names
remove "A" from names

for each name in names:
    say name without newline
end

set count to 0
forever:
    add 1 to count
    if count == 2:
        next loop
    end
    if count > 3:
        stop loop
    end
    add 10 to total
end

if total == 26 and true and nothing == nil:
    say " ok " + square(4)
else:
    say "bad"
end
"""
    compiled = compile_source(sample)
    namespace = {}
    stream = io.StringIO()
    with contextlib.redirect_stdout(stream):
        exec(compiled, namespace)
    actual = stream.getvalue().strip()
    expected = "B ok 16"
    if actual != expected:
        raise AssertionError(f"Expected {expected!r}, got {actual!r}")
    print("B++ compiler self-test passed.")


def build_arg_parser():
    parser = argparse.ArgumentParser(
        prog="b++",
        description="Compile B++ source code to Python. Use -- before arguments meant for --run output.",
    )
    parser.add_argument("source", nargs="?", help="B++ source file (.bpp/.bpm), or '-' for stdin")
    parser.add_argument("-o", "--output", help="Python output path")
    parser.add_argument("--emit", action="store_true", help="print the generated Python")
    parser.add_argument("--run", action="store_true", help="run B++ source without keeping generated Python")
    parser.add_argument("--quiet", action="store_true", help="hide compile status messages")
    parser.add_argument("--install", action="store_true", help="install B++ for the current user")
    parser.add_argument("--uninstall", action="store_true", help="remove the current-user B++ install")
    parser.add_argument("--doctor", action="store_true", help="check the B++ install")
    parser.add_argument("--update-status", action="store_true", help="show B++ update settings")
    parser.add_argument("--check-update", action="store_true", help="check GitHub for a newer B++ release")
    parser.add_argument("--update", action="store_true", help="download and install the newest B++ release")
    parser.add_argument("--force-update", action="store_true", help="install the latest release even if versions match")
    parser.add_argument("--auto", "--enable-auto-update", dest="enable_auto_update", action="store_true", help="enable periodic update checks")
    parser.add_argument("--no-auto", "--disable-auto-update", dest="disable_auto_update", action="store_true", help="disable periodic update checks")
    parser.add_argument("--update-interval-hours", type=float, help="hours between automatic update checks")
    parser.add_argument("--self-test", action="store_true", help="run the compiler smoke test")
    parser.add_argument("--version", action="version", version=f"B++ compiler {CURRENT_VERSION}")
    return parser


def main(argv=None):
    parser = build_arg_parser()
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    command_aliases = {
        "install": "--install",
        "uninstall": "--uninstall",
        "doctor": "--doctor",
        "updates": "--update-status",
        "check-update": "--check-update",
        "update": "--update",
    }
    if raw_argv and raw_argv[0] in command_aliases:
        raw_argv[0] = command_aliases[raw_argv[0]]

    program_args = []
    if "--" in raw_argv:
        separator = raw_argv.index("--")
        program_args = raw_argv[separator + 1 :]
        raw_argv = raw_argv[:separator]

    args = parser.parse_args(raw_argv)

    try:
        update_config_changed = (
            args.enable_auto_update
            or args.disable_auto_update
            or args.update_interval_hours is not None
        )
        if update_config_changed:
            config = configure_updates(
                auto_update=True if args.enable_auto_update else False if args.disable_auto_update else None,
                interval_hours=args.update_interval_hours,
            )
            print(f"Update source: {format_update_source(config)}")
            print(f"Automatic updates: {'on' if config.get('auto_update') else 'off'}")

        if args.install:
            install_bpp()
            return 0

        if args.uninstall:
            uninstall_bpp()
            return 0

        if args.doctor:
            doctor_bpp()
            return 0

        if args.update_status:
            check_update_status()
            return 0

        if args.check_update:
            check_for_update_command()
            return 0

        if args.update:
            update_bpp(force=args.force_update, quiet=args.quiet)
            return 0

        if update_config_changed and not args.source:
            check_update_status()
            return 0

    except (RuntimeError, urllib.error.URLError, json.JSONDecodeError) as exc:
        print(exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"Install error: {exc}", file=sys.stderr)
        return 1

    if args.self_test:
        run_self_test()
        return 0

    if not args.source:
        parser.print_help()
        return 2

    try:
        auto_update_if_due(quiet=args.quiet)
        source_path = None if args.source == "-" else os.path.abspath(args.source)
        src = read_source(args.source)
        compiled = compile_source(src, source_path=source_path)

        output_path = args.output
        temp_path = None

        if args.source != "-" and output_path is None and not args.run and not args.emit:
            output_path = default_output_path(args.source)

        if output_path:
            write_output(output_path, compiled)
            if not args.quiet:
                print(f"Compiled {args.source} -> {output_path}")

        if args.emit:
            print(compiled)

        if args.run:
            run_path = output_path
            if run_path is None:
                with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False, encoding="utf-8") as handle:
                    handle.write(compiled)
                    temp_path = handle.name
                run_path = temp_path
            code = run_python(run_path, program_args)
            if temp_path:
                try:
                    os.remove(temp_path)
                except OSError:
                    pass
            return code

    except CompileError as exc:
        print(exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"File error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
