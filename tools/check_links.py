"""Every file a document links to - does it exist?

    python tools/check_links.py

Exit code is the number of links that point at nothing, so this can gate a
commit. Written after R8 found six broken links that had been broken since
R1c renamed a header and R5 moved two directories: the paths were updated in
the code, and the documents were not. Nothing noticed, because nothing looked.

Only relative links are checked. http, mailto and #anchors are somebody
else's problem.
"""
import os
import re
import sys

DOCS = ("README.md", "README.th.md", "CONTRIBUTING.md", "CONTRIBUTING.th.md",
        "docs/README.md", "examples/README.md")
LINK = re.compile(r"\]\(([^)#:]+?)\)")


def main():
    bad = 0
    checked = 0
    for doc in DOCS:
        if not os.path.exists(doc):
            continue
        base = os.path.dirname(doc)
        text = open(doc, encoding="utf-8").read()
        for m in LINK.finditer(text):
            target = m.group(1)
            if target.startswith(("http", "#", "mailto")):
                continue
            checked += 1
            if not os.path.exists(os.path.normpath(os.path.join(base, target))):
                line = text[: m.start()].count("\n") + 1
                print(f"  {doc}:{line}  -> {target}")
                bad += 1
    print(f"{checked} links checked, {bad} broken")
    return bad


if __name__ == "__main__":
    sys.exit(main())
