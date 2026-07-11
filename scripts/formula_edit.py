#!/usr/bin/env python3
# Copyright (c) 2026 Brian J. Fox
# Licensed under GPLv2 with the GPLv2-AI Exception.
#
# formula_edit.py -- surgical edits to a Homebrew formula for release.sh.
#
#   formula_edit.py FORMULA.rb set-source --url URL --sha SHA256
#   formula_edit.py FORMULA.rb set-bottle --root-url URL --tag TAG --sha SHA256
#
# set-source bumps the source `url' and top-level `sha256', and drops any
# existing `bottle do ... end' block (a new bottle is added afterwards, once
# it has been built).  set-bottle inserts/replaces the bottle block right
# after the `head' line.  Edits are line-based so the rest of the formula --
# license, dependencies, install, and test blocks -- is left untouched.

import argparse
import re
import sys

BOTTLE_RE = re.compile(r'^\s*bottle\s+do\s*$')
END_RE = re.compile(r'^\s*end\s*$')
COMMENT_RE = re.compile(r'^\s*#')
URL_RE = re.compile(r'^(\s*url\s+)"[^"]*"(.*)$')
SRC_SHA_RE = re.compile(r'^(\s*)sha256\s+"[0-9a-f]{64}"\s*$')
HEAD_RE = re.compile(r'^\s*head\s+"')


def collapse_blanks(lines):
    """Collapse any run of 2+ blank lines down to a single blank line."""
    out, blank = [], False
    for ln in lines:
        if ln.strip() == "":
            if blank:
                continue
            blank = True
        else:
            blank = False
        out.append(ln)
    return out


def remove_bottle(lines):
    """Drop a `bottle do ... end' block and the comment lines just above it."""
    out, i, n = [], 0, len(lines)
    while i < n:
        if BOTTLE_RE.match(lines[i]):
            while out and COMMENT_RE.match(out[-1]):
                out.pop()
            i += 1
            while i < n and not END_RE.match(lines[i]):
                i += 1
            i += 1  # skip the matching `end'
            continue
        out.append(lines[i])
        i += 1
    return out


def set_source(lines, url, sha):
    out = []
    for line in lines:
        m = URL_RE.match(line)
        if m:
            out.append(f'{m.group(1)}"{url}"{m.group(2)}\n')
            continue
        m = SRC_SHA_RE.match(line)
        if m:
            out.append(f'{m.group(1)}sha256 "{sha}"\n')
            continue
        out.append(line)
    return collapse_blanks(remove_bottle(out))


def set_bottle(lines, root_url, tag, sha, formula):
    lines = remove_bottle(lines)
    ind = "  "
    block = [
        "\n",
        f"{ind}# Prebuilt binaries.  `brew install {formula}' uses these when one exists for the\n",
        f"{ind}# host; otherwise it falls back to building from source.  Bottles are attached\n",
        f"{ind}# to the matching release in this tap's repo.\n",
        f"{ind}bottle do\n",
        f'{ind}  root_url "{root_url}"\n',
        f'{ind}  sha256 cellar: :any_skip_relocation, {tag}: "{sha}"\n',
        f"{ind}end\n",
    ]
    out, inserted = [], False
    for line in lines:
        out.append(line)
        if not inserted and HEAD_RE.match(line):
            out.extend(block)
            inserted = True
    if not inserted:
        sys.exit("formula_edit: no `head' line found to anchor the bottle block")
    return collapse_blanks(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("formula")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("set-source")
    s.add_argument("--url", required=True)
    s.add_argument("--sha", required=True)
    b = sub.add_parser("set-bottle")
    b.add_argument("--root-url", required=True)
    b.add_argument("--tag", required=True)
    b.add_argument("--sha", required=True)
    b.add_argument("--formula-name", default="gnash")
    args = ap.parse_args()

    with open(args.formula) as f:
        lines = f.readlines()

    if args.cmd == "set-source":
        if len(args.sha) != 64:
            sys.exit("set-source: --sha must be a 64-char sha256")
        lines = set_source(lines, args.url, args.sha)
    else:
        if len(args.sha) != 64:
            sys.exit("set-bottle: --sha must be a 64-char sha256")
        lines = set_bottle(lines, args.root_url, args.tag, args.sha, args.formula_name)

    with open(args.formula, "w") as f:
        f.writelines(lines)


if __name__ == "__main__":
    main()
