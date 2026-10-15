"""Parser of the HRL public C headers (hrl.h, hrl_gl.h, hrl_vulkan.h).

No dependency : the headers follow a regular C style (typedef enum/struct,
HRL_API functions, #define constants), parsed here into a small intermediate
representation (IR) consumed by the language emitters.
"""
import re

# --------------------------------------------------------------------------
# IR
# --------------------------------------------------------------------------


class CType:
    """A C type : base name + pointer depth (+ const on the pointee)."""

    def __init__(self, base, pointers=0, const=False):
        self.base = base
        self.pointers = pointers
        self.const = const

    def __repr__(self):
        return ("const " if self.const else "") + self.base + "*" * self.pointers

    def is_void(self):
        return self.base == "void" and self.pointers == 0

    def to_json(self):
        return {"base": self.base, "pointers": self.pointers, "const": self.const}


class Param:
    def __init__(self, name, ctype, array=None):
        self.name = name
        self.type = ctype
        self.array = array  # fixed array length for struct fields


class Function:
    def __init__(self, name, ret, params, doc, header, variadic=False):
        self.name = name
        self.ret = ret
        self.params = params
        self.doc = doc
        self.header = header
        self.variadic = variadic


class Enum:
    def __init__(self, name, values, doc):
        self.name = name
        self.values = values  # list of (name, int value, doc)
        self.doc = doc


class Struct:
    def __init__(self, name, fields, doc, opaque=False):
        self.name = name
        self.fields = fields
        self.doc = doc
        self.opaque = opaque


class Callback:
    def __init__(self, name, ret, params, doc):
        self.name = name
        self.ret = ret
        self.params = params
        self.doc = doc


class Constant:
    def __init__(self, name, kind, value, doc, ctype=None):
        self.name = name
        self.kind = kind  # "int", "uint", "float", "string"
        self.value = value
        self.doc = doc
        self.ctype = ctype  # explicit C type of a cast, e.g. HRL_id


class API:
    def __init__(self):
        self.version = ""
        self.constants = []
        self.aliases = {}  # typedef name -> CType (scalar aliases)
        self.enums = []
        self.structs = []
        self.callbacks = []
        self.functions = []
        self.skipped = []  # (name, reason)

    # helpers ------------------------------------------------------------
    def int_names(self):
        """name -> value of every integer constant and enumerator (fixed array sizes)."""
        d = {c.name: c.value for c in self.constants}
        for e in self.enums:
            for n, v, _ in e.values:
                d[n] = v
        return d

    def enum(self, name):
        return next((e for e in self.enums if e.name == name), None)

    def struct(self, name):
        return next((s for s in self.structs if s.name == name), None)

    def callback(self, name):
        return next((c for c in self.callbacks if c.name == name), None)

    def resolve(self, ctype):
        """Follows scalar typedefs (HRL_id -> uint32_t)."""
        seen = 0
        t = ctype
        while t.base in self.aliases and seen < 8:
            a = self.aliases[t.base]
            t = CType(a.base, t.pointers + a.pointers, t.const or a.const)
            seen += 1
        return t

    def to_json(self):
        def doc(d):
            return d or ""
        return {
            "version": self.version,
            "constants": [{"name": c.name, "kind": c.kind, "value": c.value, "type": c.ctype, "doc": doc(c.doc)} for c in self.constants],
            "aliases": {k: v.to_json() for k, v in self.aliases.items()},
            "enums": [{"name": e.name, "doc": doc(e.doc), "values": [{"name": n, "value": v, "doc": doc(d)} for n, v, d in e.values]} for e in self.enums],
            "structs": [{"name": s.name, "opaque": s.opaque, "doc": doc(s.doc),
                         "fields": [{"name": f.name, "type": f.type.to_json(), "array": f.array} for f in s.fields]} for s in self.structs],
            "callbacks": [{"name": c.name, "doc": doc(c.doc), "return": c.ret.to_json(),
                           "params": [{"name": p.name, "type": p.type.to_json()} for p in c.params]} for c in self.callbacks],
            "functions": [{"name": f.name, "header": f.header, "doc": doc(f.doc), "return": f.ret.to_json(), "variadic": f.variadic,
                           "params": [{"name": p.name, "type": p.type.to_json()} for p in f.params]} for f in self.functions],
            "skipped": [{"name": n, "reason": r} for n, r in self.skipped],
        }


# --------------------------------------------------------------------------
# Preprocessor (conditionals + object-like #define)
# --------------------------------------------------------------------------

def _eval_condition(expr, defined):
    expr = expr.split("//")[0].strip()
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)", lambda m: "1" if m.group(1) in defined else "0", expr)
    expr = re.sub(r"defined\s+(\w+)", lambda m: "1" if m.group(1) in defined else "0", expr)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    expr = re.sub(r"\b[A-Za-z_]\w*\b", lambda m: m.group(0) if m.group(0) in ("and", "or", "not") else "0", expr)
    try:
        return bool(eval(expr, {}, {}))
    except Exception:
        return False


def preprocess(text, defines):
    """Returns (code lines kept, list of (name, value, doc)) for a C build
    (no __cplusplus, no _WIN32 : HRL_API expands to nothing)."""
    lines = text.splitlines()
    out = []
    macros = []
    stack = []  # (active, taken)
    active = True
    pending_doc = None
    i = 0
    while i < len(lines):
        line = lines[i]
        # join continuation lines
        while line.rstrip().endswith("\\") and i + 1 < len(lines):
            i += 1
            line = line.rstrip()[:-1] + " " + lines[i]
        stripped = line.strip()
        m = re.match(r"#\s*(\w+)\s*(.*)", stripped)
        if m:
            d, rest = m.group(1), m.group(2)
            if d in ("ifdef", "ifndef", "if"):
                if d == "ifdef":
                    cond = rest.split()[0] in defines
                elif d == "ifndef":
                    cond = rest.split()[0] not in defines
                else:
                    cond = _eval_condition(rest, defines)
                stack.append((active, cond))
                active = active and cond
            elif d == "elif":
                parent, taken = stack[-1]
                cond = (not taken) and _eval_condition(rest, defines)
                stack[-1] = (parent, taken or cond)
                active = parent and cond
            elif d == "else":
                parent, taken = stack[-1]
                stack[-1] = (parent, True)
                active = parent and not taken
            elif d == "endif":
                active = stack.pop()[0]
            elif d == "define" and active:
                dm = re.match(r"(\w+)(\([^)]*\))?\s*(.*)", rest)
                name, func, value = dm.group(1), dm.group(2), dm.group(3)
                defines.add(name)
                if not func:
                    value = re.sub(r"/\*.*?\*/", "", value).split("//")[0].strip()
                    macros.append((name, value, pending_doc))
                pending_doc = None
            out.append("")
            i += 1
            continue
        out.append(line if active else "")
        i += 1
    return "\n".join(out), macros


# --------------------------------------------------------------------------
# Tokenizer
# --------------------------------------------------------------------------

_TOKEN = re.compile(r"""
    (?P<doc>/\*\*(?!/).*?\*/)
  | (?P<comment>/\*.*?\*/|//[^\n]*)
  | (?P<string>"(?:\\.|[^"\\])*")
  | (?P<number>0[xX][0-9a-fA-F]+[uUlL]*|\d+\.\d*(?:[eE][-+]?\d+)?[fF]?|\d+[uUlL]*)
  | (?P<ident>[A-Za-z_]\w*)
  | (?P<op><<|>>|[{}()\[\];,*=|&+\-~<>!/.])
  | (?P<ws>\s+)
""", re.S | re.X)


def tokenize(text):
    tokens = []
    pos = 0
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m:
            raise SyntaxError("unexpected character %r at %d" % (text[pos], pos))
        pos = m.end()
        kind = m.lastgroup
        if kind in ("ws", "comment"):
            continue
        tokens.append((kind, m.group(kind)))
    return tokens


def clean_doc(raw):
    if not raw:
        return None
    body = raw[3:-2]
    lines = []
    for l in body.splitlines():
        l = l.strip()
        if l.startswith("*"):
            l = l[1:]
            if l.startswith(" "):
                l = l[1:]
        lines.append(l.rstrip())
    while lines and not lines[0]:
        lines.pop(0)
    while lines and not lines[-1]:
        lines.pop()
    text = "\n".join(lines).strip()
    return text or None


# --------------------------------------------------------------------------
# Expressions (enum values, #define constants)
# --------------------------------------------------------------------------

def eval_int(expr, names):
    e = expr.strip()
    e = re.sub(r"\(\s*(?:int|unsigned|HRL_\w+|uint\d+_t|int\d+_t)\s*\)", "", e)  # casts
    e = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", e)
    e = re.sub(r"\b([A-Za-z_]\w*)\b", lambda m: str(names[m.group(1)]) if m.group(1) in names else m.group(1), e)
    if re.search(r"[A-Za-z_]", re.sub(r"0[xX][0-9a-fA-F]+", "", e)):
        raise ValueError("unresolved expression: " + expr)
    return int(eval(e, {}, {}))


# --------------------------------------------------------------------------
# Declarations
# --------------------------------------------------------------------------

_QUALIFIERS = {"extern", "static", "inline", "struct", "enum", "unsigned", "signed"}


def parse_type_and_name(tokens):
    """[const] base [*...] name [ [N] ] -> (CType, name, array expr)."""
    toks = [t for t in tokens]
    array = None
    if "[" in [v for _, v in toks]:
        k = [v for _, v in toks].index("[")
        array = " ".join(v for _, v in toks[k + 1:-1])
        toks = toks[:k]
    const = False
    words = []
    pointers = 0
    for kind, v in toks:
        if v == "const":
            if not words:
                const = True
            continue
        if v == "*":
            pointers += 1
            continue
        words.append(v)
    if not words:
        raise ValueError("empty declaration")
    name = None
    if len(words) >= 2 and words[-1] not in ("int", "char", "short", "long", "float", "double"):
        name = words[-1]
        words = words[:-1]
    base_words = [w for w in words if w not in ("struct", "enum")]
    base = " ".join(base_words)
    base = {"unsigned": "unsigned int", "unsigned char": "unsigned char", "signed char": "signed char"}.get(base, base)
    return CType(base, pointers, const), name, array


def split_params(tokens):
    params = []
    depth = 0
    cur = []
    for t in tokens:
        if t[1] == "(":
            depth += 1
        elif t[1] == ")":
            depth -= 1
        if t[1] == "," and depth == 0:
            params.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        params.append(cur)
    return params


def parse_header(text, header_name, api, defines):
    code, macros = preprocess(text, defines)
    tokens = [t for t in tokenize(code) if t[1] != "HRL_API"]
    macro_docs = {}
    # Statements ------------------------------------------------------------
    i = 0
    doc = None
    n = len(tokens)
    while i < n:
        kind, v = tokens[i]
        if kind == "doc":
            doc = clean_doc(v)
            i += 1
            continue
        if v == "extern" and i + 1 < n and tokens[i + 1][0] == "string":
            i += 2
            if i < n and tokens[i][1] == "{":
                i += 1
            continue
        if v == "}":  # end of extern "C"
            i += 1
            continue
        # collect a statement up to ';' at depth 0 (or a function body)
        start = i
        depth = 0
        body = False
        while i < n:
            t = tokens[i][1]
            if t in "({[":
                depth += 1
            elif t in ")}]":
                depth -= 1
                if t == "}" and depth == 0 and tokens[start][1] in ("static", "inline"):
                    body = True
                    i += 1
                    break
            elif t == ";" and depth == 0:
                i += 1
                break
            i += 1
        stmt = [t for t in tokens[start:i] if t[0] != "doc"]
        inner_docs = tokens[start:i]  # with the doc comments (enumerators)
        if body:
            name = next((tv for tk, tv in stmt if tk == "ident" and tv.startswith("HRL_")), "?")
            api.skipped.append((name, "static inline function (header only)"))
            doc = None
            continue
        if stmt and stmt[-1][1] == ";":
            stmt = stmt[:-1]
        if stmt and stmt[0][1] in ("_Static_assert", "static_assert"):
            doc = None
            continue
        if not stmt:
            doc = None
            continue
        try:
            handle_statement(stmt, inner_docs, doc, header_name, api)
        except Exception as e:  # pragma: no cover - reported to the user
            raise SyntaxError("%s: cannot parse '%s': %s" % (header_name, " ".join(v for _, v in stmt)[:160], e))
        doc = None

    # Constants -------------------------------------------------------------
    names = {"UINT32_MAX": 0xFFFFFFFF, "INT32_MAX": 0x7FFFFFFF, "UINT8_MAX": 0xFF, "UINT16_MAX": 0xFFFF}
    for e in api.enums:
        for nm, val, _ in e.values:
            names[nm] = val
    for name, value, mdoc in macros:
        if name in ("HRL_IMPL", "HRL_API") or name.endswith("_H") or not value:
            continue
        if name == "HRL_API_VERSION":
            api.version = value.strip('"')
        if value.startswith('"'):
            api.constants.append(Constant(name, "string", value.strip('"'), mdoc))
            continue
        cast = re.match(r"\(\s*\(\s*(\w+)\s*\)\s*(.*)\)$", value)
        try:
            if cast:
                v = eval_int(cast.group(2), names)
                ctype = cast.group(1)
                if ctype in ("int",):
                    api.constants.append(Constant(name, "int", v, mdoc))
                else:
                    resolved = api.resolve(CType(ctype)).base
                    if v < 0 and resolved.startswith("uint"):
                        bits = int(re.search(r"\d+", resolved).group(0))
                        v &= (1 << bits) - 1
                    api.constants.append(Constant(name, "uint", v, mdoc, ctype))
                names[name] = v
                continue
            if re.match(r"^-?\d+\.\d*[fF]?$", value):
                api.constants.append(Constant(name, "float", float(value.rstrip("fF")), mdoc))
                continue
            v = eval_int(value, names)
            kind = "uint" if re.search(r"\d[uU]|UINT\d+_MAX", value) else "int"
            api.constants.append(Constant(name, kind, v, mdoc))
            names[name] = v
        except Exception:
            api.skipped.append((name, "macro is not a constant: " + value))


def handle_statement(stmt, inner_docs, doc, header, api):
    vals = [v for _, v in stmt]
    if vals[0] == "typedef":
        body = stmt[1:]
        bv = vals[1:]
        if bv[0] == "enum":
            name = bv[-1]
            raw = inner_docs[1:]
            rv = [v for _, v in raw]
            body_raw = raw[rv.index("{") + 1:len(rv) - 1 - rv[::-1].index("}")]
            values = []
            names = {e2[0]: e2[1] for en in api.enums for e2 in en.values}
            current = -1
            # enumerator docs : a doc comment precedes the enumerator
            items = split_enum(body_raw, None)
            for ename, expr, edoc in items:
                current = eval_int(expr, names) if expr else current + 1
                names[ename] = current
                values.append((ename, current, edoc))
            api.enums.append(Enum(name, values, doc))
            return
        if bv[0] == "struct":
            if "{" not in bv:
                # typedef struct X X; : opaque
                api.structs.append(Struct(bv[-1], [], doc, opaque=True))
                return
            open_ = bv.index("{")
            close = len(bv) - 1 - bv[::-1].index("}")
            fields = []
            for decl in split_fields(body[open_ + 1:close]):
                ctype, fname, array = parse_type_and_name(decl)
                fields.append(Param(fname, ctype, array))
            api.structs.append(Struct(bv[-1], fields, doc))
            return
        if "(" in bv and bv[bv.index("(") + 1] == "*":
            # function pointer typedef
            p = bv.index("(")
            ret, _, _ = parse_type_and_name([("ident", "x")] if False else body[:p] + [("ident", "_r")])
            name = bv[p + 2]
            q = p + 3
            assert bv[q] == ")"
            args_open = q + 1
            args = body[args_open + 1:len(body) - 1]
            params = parse_params(args)
            api.callbacks.append(Callback(name, ret, params, doc))
            return
        ctype, name, _ = parse_type_and_name(body)
        api.aliases[name] = ctype
        return
    if "(" in vals:
        p = vals.index("(")
        ret, name, _ = parse_type_and_name(stmt[:p])
        args = stmt[p + 1:len(stmt) - 1]
        variadic = False
        av = [v for _, v in args]
        if len(av) >= 3 and av[-3:] == [".", ".", "."]:
            variadic = True
            args = args[:-3]
            if args and args[-1][1] == ",":
                args = args[:-1]
        params = parse_params(args)
        api.functions.append(Function(name, ret, params, doc, header, variadic))
        return
    api.skipped.append((" ".join(vals)[:60], "unknown declaration"))


def parse_params(tokens):
    params = []
    if not tokens or [v for _, v in tokens] == ["void"]:
        return params
    for i, ptoks in enumerate(split_params(tokens)):
        ctype, name, array = parse_type_and_name(ptoks)
        if array is not None:
            ctype = CType(ctype.base, ctype.pointers + 1, ctype.const)
        params.append(Param(name or "arg%d" % i, ctype))
    return params


def split_fields(tokens):
    fields = []
    cur = []
    for t in tokens:
        if t[0] == "doc":
            continue
        if t[1] == ";":
            if cur:
                fields.append(cur)
            cur = []
        else:
            cur.append(t)
    return fields


def split_enum(tokens, docs):
    # docs inside the enum body are kept in order in the token stream of the
    # statement; re-tokenizing is avoided by matching enumerators in order.
    items = []
    cur = []
    pending = None
    for t in tokens:
        if t[0] == "doc":
            if cur:  # trailing doc of the previous enumerator
                items.append((cur, pending))
                cur = []
                pending = None
            pending = clean_doc(t[1])
            continue
        if t[1] == ",":
            if cur:
                items.append((cur, pending))
                pending = None
            cur = []
        else:
            cur.append(t)
    if cur:
        items.append((cur, pending))
    out = []
    for it, d in items:
        name = it[0][1]
        expr = None
        if len(it) > 2 and it[1][1] == "=":
            expr = " ".join(v for _, v in it[2:])
        out.append((name, expr, d))
    return out


def parse(headers):
    """headers : list of (file name, text). Returns API."""
    api = API()
    defines = set()
    for name, text in headers:
        parse_header(text, name, api, defines)
    return api


# --------------------------------------------------------------------------
# Parameter roles (shared by the emitters)
# --------------------------------------------------------------------------
SCALARS = {
    "int": ("int32", True), "unsigned int": ("uint32", True), "float": ("float32", True), "double": ("float64", True),
    "char": ("int8", True), "unsigned char": ("uint8", True), "short": ("int16", True),
    "int8_t": ("int8", True), "uint8_t": ("uint8", True), "int16_t": ("int16", True), "uint16_t": ("uint16", True),
    "int32_t": ("int32", True), "uint32_t": ("uint32", True), "int64_t": ("int64", True), "uint64_t": ("uint64", True),
    "size_t": ("usize", True), "bool": ("bool", True),
}


def scalar_kind(api, ctype):
    """Portable scalar name of a non-pointer type (enums are int32), or None."""
    t = api.resolve(ctype)
    if t.pointers:
        return None
    if t.base in SCALARS:
        return SCALARS[t.base][0]
    if api.enum(t.base):
        return "enum"
    return None


BINARY_NAMES = ("data", "content", "buffer", "bytes")


def param_roles(api, func):
    """Role of every parameter :
      value       scalar / enum passed by value
      string      const char* text
      bytes       const char* / const void* binary data followed by a size
      size        the size_t length of the previous bytes/array parameter
      out         non-const pointer to one scalar / enum (output)
      out_matrix  float* aa : 16 floats written by the function
      out_string  const char** written by the function
      out_struct  non-const pointer to a struct the function fills
      array       const T* input array followed by its count
      strings     const char** input array followed by its count
      callback    function pointer typedef
      pointer     anything else (handles, struct pointers, void*)
    """
    roles = []
    params = func.params
    for i, p in enumerate(params):
        t = api.resolve(p.type)
        nxt = params[i + 1] if i + 1 < len(params) else None
        nxt_is_size = nxt is not None and api.resolve(nxt.type).pointers == 0 and api.resolve(nxt.type).base in ("size_t", "uint32_t", "int", "unsigned int")
        lname = (p.name or "").lower()
        if roles and roles[-1] in ("bytes", "array", "strings") and t.pointers == 0 and t.base in ("size_t", "uint32_t", "int", "unsigned int") and i > 0:
            roles.append("size")
            continue
        if api.callback(p.type.base) and p.type.pointers == 0:
            roles.append("callback")
        elif t.pointers == 0:
            roles.append("value")
        elif t.pointers == 1 and t.base == "char" and t.const:
            roles.append("bytes" if nxt_is_size and any(b in lname for b in BINARY_NAMES) and api.resolve(nxt.type).base == "size_t" else "string")
        elif t.pointers == 1 and t.base == "void" and t.const and nxt_is_size:
            roles.append("bytes")
        elif t.pointers == 2 and t.base == "char" and t.const:
            roles.append("strings" if nxt_is_size else "out_string")
        elif t.pointers == 1 and not t.const and lname == "aa" and t.base == "float":
            roles.append("out_matrix")
        elif t.pointers == 1 and not t.const and scalar_kind(api, CType(t.base)) is not None:
            roles.append("out")
        elif t.pointers == 1 and not t.const and api.struct(t.base) and not api.struct(t.base).opaque and func.name.startswith(("HRL_Voxel", "HRL_Get")) and "out" in lname:
            roles.append("out_struct")
        elif t.pointers == 1 and t.const and (scalar_kind(api, CType(t.base)) is not None or (api.struct(t.base) and not api.struct(t.base).opaque)) and _array_count_follows(api, params, i):
            roles.append("array")
        else:
            roles.append("pointer")
    return roles


def _array_count_follows(api, params, i):
    # const T* a, const T* b, ..., size_t count : parallel arrays share the count
    j = i + 1
    while j < len(params):
        t = api.resolve(params[j].type)
        if t.pointers == 1 and t.const:
            j += 1
            continue
        return t.pointers == 0 and t.base in ("size_t", "uint32_t", "int", "unsigned int")
    return False


def clean_name(name):
    """_sceneid -> sceneid"""
    n = (name or "arg").lstrip("_")
    return n or "arg"
