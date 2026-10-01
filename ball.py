#!/usr/bin/env python3
"""Launch lilJack's native apps with the operator's environment unchanged.

python3 ball.py liljack [args] forwards to ./liljack [args].
python3 ball.py owkterm forwards to ./liljack owkterm.
Both use exec so shell job control follows the launched app.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main(argv):
    if len(argv) >= 2 and argv[1] in ('liljack', 'owkterm'):
        launcher = os.path.join(HERE, 'liljack')
        os.execv(launcher, [launcher, *([] if argv[1] == "liljack" else ["owkterm"]), *argv[2:]])
        raise SystemExit('exec failed')                     # only if execv returns
    sys.stderr.write(
        "lilJack's ball.py knows `liljack` and `owkterm`: python3 ball.py liljack [--tui ...]\n"
        )
    return 2


if __name__ == '__main__':
    raise SystemExit(main(sys.argv))
