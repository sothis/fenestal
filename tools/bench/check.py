#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Janos Sonntag
"""Check fenestal-bench/1 records against tools/bench/schema.json.

    python3 tools/bench/check.py <record.json>...

Says each problem on a line, with where in the document it is, and exits 1
if there was any. No dependency: the schema is read as data and checked by
the few JSON Schema keywords it uses (type, const, enum, required,
properties, additionalProperties, propertyNames, items, pattern, minimum,
anyOf, $ref into the document), which keeps it the one definition of the record.
What a schema cannot say is checked after it: a series row as wide as its
columns, and a status that agrees with its reasons.
"""

import json
import os
import re
import sys

SCHEMA_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "schema.json")

_TYPES = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
    # JSON's bool is not a number, and Python's is.
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float))
    and not isinstance(v, bool),
}


def load_schema():
    with open(SCHEMA_PATH) as f:
        return json.load(f)


def _resolve(root, ref):
    if not ref.startswith("#/"):
        raise ValueError("only local references: %s" % ref)
    node = root
    for part in ref[2:].split("/"):
        node = node[part]
    return node


def _likeness(root, option, value, trial):
    """How nearly `value` is of the form `option`: how deep its problems
    lie, then how many of the form's required keys it has."""
    while "$ref" in option:
        option = _resolve(root, option["$ref"])
    depth = max(len(p.split(":")[0]) for p in trial)
    keys = sum(k in value for k in option.get("required", ())) \
        if isinstance(value, dict) else 0
    return depth, keys


def _check(root, schema, value, where, out):
    """Appends to `out` what is wrong with `value` under `schema`."""
    if "$ref" in schema:
        _check(root, _resolve(root, schema["$ref"]), value, where, out)
        return
    if "anyOf" in schema:
        trials = []
        for option in schema["anyOf"]:
            trial = []
            _check(root, option, value, where, trial)
            if not trial:
                return
            trials.append((_likeness(root, option, value, trial), trial))
        # None fits: say why the form it was meant to be does not.
        out.extend(max(trials, key=lambda t: t[0])[1])
        return
    if "const" in schema and value != schema["const"]:
        out.append("%s: is %r, not %r" % (where, value, schema["const"]))
    if "enum" in schema and value not in schema["enum"]:
        out.append("%s: is %r, not one of %s"
                   % (where, value, ", ".join(map(str, schema["enum"]))))
    if "type" in schema:
        kinds = schema["type"]
        kinds = [kinds] if isinstance(kinds, str) else kinds
        if not any(_TYPES[k](value) for k in kinds):
            out.append("%s: is %s, not %s"
                       % (where, type(value).__name__, " or ".join(kinds)))
            return
    if isinstance(value, str) and "pattern" in schema and \
            not re.search(schema["pattern"], value):
        out.append("%s: %r does not match %s"
                   % (where, value, schema["pattern"]))
    if _TYPES["number"](value) and "minimum" in schema and \
            value < schema["minimum"]:
        out.append("%s: %r is below %r" % (where, value, schema["minimum"]))
    if isinstance(value, dict):
        for key in schema.get("required", ()):
            if key not in value:
                out.append("%s: no %r" % (where, key))
        props = schema.get("properties", {})
        names = schema.get("propertyNames")
        extra = schema.get("additionalProperties")
        for key, v in value.items():
            if names is not None:
                _check(root, names, key, "%s key %r" % (where, key), out)
            if key in props:
                _check(root, props[key], v, "%s.%s" % (where, key), out)
            elif isinstance(extra, dict):
                _check(root, extra, v, "%s.%s" % (where, key), out)
    if isinstance(value, list) and "items" in schema:
        for i, v in enumerate(value):
            _check(root, schema["items"], v, "%s[%d]" % (where, i), out)


def problems(doc, schema=None):
    """What is wrong with one record, as lines; none when it is sound."""
    schema = schema or load_schema()
    out = []
    _check(schema, schema, doc, "record", out)
    if out:
        return out
    for name in ("series", "frames", "library_frames", "shown_frames"):
        table = doc.get(name)
        if not table:
            continue
        width = len(table["columns"])
        for i, row in enumerate(table["rows"]):
            if len(row) != width:
                out.append("record.%s.rows[%d]: %d values for %d columns"
                           % (name, i, len(row), width))
    run = doc["run"]
    if run["status"] == "complete" and run["invalid_reasons"]:
        out.append("record.run: complete, with reasons it is invalid")
    if run["status"] == "invalid" and not run["invalid_reasons"]:
        out.append("record.run: invalid, with no reason given")
    return out


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0 if argv else 1
    schema = load_schema()
    bad = 0
    for path in argv:
        try:
            with open(path) as f:
                doc = json.load(f)
        except (OSError, ValueError) as e:
            print("%s: %s" % (path, e))
            bad += 1
            continue
        found = problems(doc, schema)
        for line in found:
            print("%s: %s" % (path, line))
        bad += bool(found)
    if bad:
        print("%d of %d records have problems" % (bad, len(argv)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
