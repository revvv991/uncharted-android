#!/usr/bin/env bash
# make_public_copy.sh <out dir>: the public repository as it would be published, made locally
# (stage 6 of the plan). Nothing is pushed and no remote is set: publishing is the owner's decision
# alone (the fork of Supermedo/uncharted_pc, then a push of this copy's main).
#
# - History: upstream's commits up to BASE (uncharted_pc 0.3.6, the same hashes as on GitHub) and
#   our work as one commit on top, "uncharted-android <version>" (android/VERSION): author and
#   committer revvv991 (GitHub noreply), the date in UTC, no trailers. Nothing of the private
#   history goes out. PREVIOUS=<directory or URL of the public repository>: a later release goes
#   on top of that repository's main instead of BASE.
# - MERGED=<commit>: an upstream commit our main has merged (uncharted_pc 0.3.9: 754ed2a) becomes
#   the release commit's second parent, so the public history carries upstream's commits up to it
#   with their authors (the same hashes as on GitHub). Left out when the parent already has it.
# - The tree: SRC's main (default: the WSL working copy) without CLAUDE.md, android/docs/ and
#   android/notes/ (the development notes) and any extra PATHS (space separated). Dropped as well:
#   the "Date:" lines of our patch files (git format-patch; the time zone tells where the author
#   lives) and the commit column of the port's change list (patches/README.md: those commits stay
#   private).
# - REPLACE=<file>: "literal==>replacement" rules, one per line, applied to every text file of the
#   tree - for strings that must not be published. The file stays outside the repository.
# - A report: the commits and authors, files made from the game, big blobs, Expected bytes left in
#   patch XML, development notes left, personal-looking lines added by our commit.
set -euo pipefail
OUT=${1:?usage: make_public_copy.sh <out dir>}
SRC=${SRC:-$HOME/src/uncharted-android}
BASE=${BASE:-b787c1068c3a9a9ba3ec43dd8086af2bf7d35c7d}
NAME=revvv991
EMAIL=345025393+revvv991@users.noreply.github.com
[ ! -e "$OUT" ] || { echo "$OUT exists: give a new directory" >&2; exit 1; }
git -C "$SRC" merge-base --is-ancestor "$BASE" main || { echo "BASE $BASE is not an ancestor of main" >&2; exit 1; }
if [ -n "${MERGED:-}" ]; then
    git -C "$SRC" merge-base --is-ancestor "$MERGED" main || { echo "MERGED $MERGED is not in main" >&2; exit 1; }
fi
VERSION=$(git -C "$SRC" show main:android/VERSION | tr -d '[:space:]')

git clone -q --no-local --no-tags --single-branch --branch main "$SRC" "$OUT"
cd "$OUT"
git remote remove origin
TREE=$(git rev-parse 'main^{tree}')
PARENT=$BASE
if [ -n "${PREVIOUS:-}" ]; then
    git fetch -q --no-tags "$PREVIOUS" main
    PARENT=$(git rev-parse FETCH_HEAD)
fi
git checkout -q --detach "$PARENT"
git read-tree -u --reset "$TREE"
git rm -r -q -f --ignore-unmatch -- CLAUDE.md android/docs android/notes ${PATHS:-}

python3 -I - "${REPLACE:-}" <<'EOF'
import os, subprocess, sys
# Our patch files without their "Date:" header lines.
for d, _, files in os.walk('android/scripts/port-arm64/patches'):
    for f in files:
        if f.endswith('.patch'):
            p = os.path.join(d, f)
            lines = open(p, encoding='utf-8', newline='').read().split('\n')
            open(p, 'w', encoding='utf-8', newline='').write('\n'.join(l for l in lines if not l.startswith('Date: ')))
# The port's change list without the commit column: those commits are not published.
p = 'android/scripts/port-arm64/patches/README.md'
s = open(p, encoding='utf-8', newline='').read()
intro = ("The port's patches are no longer files here: they are commits of this repository on top of upstream\n"
         "`b787c10` (uncharted_pc 0.3.6). The numbers the plan and the findings log refer to:\n")
if intro not in s:
    sys.exit(p + ': the intro of the change list changed, update make_public_copy.sh')
s = s.replace(intro, "The port's changes on top of upstream `b787c10` (uncharted_pc 0.3.6), in the order they were made\n"
                     "(in this repository they come as one commit per release). Changes in the code are marked\n"
                     "`g2-arm64:`.\n")
out = []
for l in s.split('\n'):
    parts = l.split(' | ')
    if l == '| # | Commit | What |':
        l = '| # | What |'
    elif l == '|---|---|---|':
        l = '|---|---|'
    elif l.startswith('| 0') and len(parts) >= 3 and len(parts[1]) == 7 and all(c in '0123456789abcdef' for c in parts[1]):
        l = ' | '.join(parts[:1] + parts[2:])
    elif l.startswith('The whole list: `git log'):
        continue
    out.append(l)
open(p, 'w', encoding='utf-8', newline='').write('\n'.join(out))
# REPLACE rules over every text file.
rules = []
if sys.argv[1]:
    for line in open(sys.argv[1], encoding='utf-8').read().split('\n'):
        if '==>' in line:
            rules.append(tuple(line.split('==>', 1)))
if rules:
    for f in subprocess.run(['git', 'ls-files', '-z'], capture_output=True, check=True).stdout.split(b'\0'):
        if not f or not os.path.isfile(f):
            continue
        data = open(f, 'rb').read()
        if b'\0' in data:
            continue
        new = data
        for old, rep in rules:
            new = new.replace(old.encode(), rep.encode())
        if new != data:
            open(f, 'wb').write(new)
EOF

git add -A
DATE=$(date -u +%Y-%m-%dT%H:%M:%S+0000)
export GIT_AUTHOR_NAME=$NAME GIT_AUTHOR_EMAIL=$EMAIL GIT_AUTHOR_DATE=$DATE
export GIT_COMMITTER_NAME=$NAME GIT_COMMITTER_EMAIL=$EMAIL GIT_COMMITTER_DATE=$DATE
PARENTS=(-p HEAD)
if [ -n "${MERGED:-}" ] && ! git merge-base --is-ancestor "$MERGED" HEAD; then
    PARENTS+=(-p "$MERGED")
fi
COMMIT=$(git commit-tree "$(git write-tree)" "${PARENTS[@]}" -m "uncharted-android $VERSION")
git branch -f main "$COMMIT"
git checkout -q -f main
git tag -l | xargs -r git tag -d > /dev/null
rm -f .git/FETCH_HEAD
git reflog expire --expire=now --all
git gc -q --prune=now

echo "== $(git rev-list --count HEAD) commits, HEAD $(git rev-parse --short HEAD) \"$(git log -1 --format=%s)\" on $(git log -1 --format=%p)"
echo "== our commit: $(git log -1 --format='author %an <%ae> %ad, committer %cn <%ce> %cd' --date=iso)"
echo "== objects outside main (should be 0): $(( $(git rev-list --objects --all | wc -l) - $(git rev-list --objects main | wc -l) ))"
echo "== authors and committers"
git log --format='%an <%ae>%n%cn <%ce>' | sort | uniq -c | sort -rn
echo "== files made from the game (should be none)"
git ls-tree -r --name-only HEAD |
    grep -iE '(^|/)(eboot\.bin|boot(-linked|-libc)?\.bin|content\.bin|param\.sfo|[^/]*\.(elf|prx|self|sprx|spv|pkg))$' || echo none
echo "== blobs over 2 MB"
git rev-list --objects --all | git cat-file --batch-check='%(objecttype) %(objectsize) %(rest)' |
    awk '$1 == "blob" && $2 > 2097152 {printf "%8.1f MB %s\n", $2 / 1048576, $3}' | sort -rn | head -20
echo "== Expected bytes in patch XML (should be none)"
git grep -l 'Expected="' HEAD -- '*.xml' || echo none
echo "== development notes (should be none)"
git ls-tree -r --name-only HEAD | grep -E '^(CLAUDE\.md|android/(docs|notes)/)' || echo none
echo "== personal-looking lines added by our commit (check each)"
patterns=('/home/[a-z][a-z0-9_-]*/' '[A-Za-z]:\\Users\\' '/Users/[A-Za-z]' '192\.168\.[0-9]' '10\.[0-9]+\.[0-9]+\.[0-9]+:'
          '[A-Za-z0-9._%+-]+@(gmail|yandex|mail|outlook|hotmail|icloud|proton)[A-Za-z.]*')
if [ -n "${REPLACE:-}" ]; then
    while IFS= read -r line; do
        lit=${line%%==>*}
        [ -n "$lit" ] && patterns+=("$(printf '%s' "$lit" | sed 's/[][\.*^$/]/\\&/g')")
    done < "$REPLACE"
fi
added=$(git diff "HEAD~1" HEAD | grep -E '^\+' || true)
for re in "${patterns[@]}"; do
    hits=$(printf '%s\n' "$added" | grep -E -- "$re" | head -5 || true)
    [ -n "$hits" ] && printf -- '-- /%s/\n%s\n' "$re" "$hits"
done
echo "== done: $OUT (no remote; publish only with the owner's decision)"
