#!/usr/bin/env python3
"""The release of the public repository through the GitHub API, on the owner's word (every push and
release needs it). The version is android/VERSION; the tag v<version>, the title
"uncharted-android <version>", the asset uncharted-android-<version>.apk. The token comes from
git's credential helper (Git Credential Manager, account revvv991) and is never printed.
  publish.py check                       the token's user and scopes, the repositories
  publish.py release <notes.md> <apk>    the pre-release with the APK (after the public push)
  publish.py verify                      the public main, the release and its assets
  publish.py describe                    the public repository's description
Run it with `python -I` from a directory that holds nothing untrusted. The steps around it:
.claude/skills/release/SKILL.md."""
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = open(os.path.join(HERE, '..', '..', 'VERSION'), encoding='utf-8').read().strip()
OWNER = 'revvv991'
PRIVATE, PUBLIC = 'uncharted-android-dev', 'uncharted-android'
TAG, TITLE, ASSET = 'v' + VERSION, 'uncharted-android ' + VERSION, 'uncharted-android-%s.apk' % VERSION
DESCRIPTION = ('Uncharted: The Nathan Drake Collection (PS4) on Android with Adreno GPUs: '
               'uncharted_pc on FEXCore and Turnip. No game files included.')


def token():
    env = dict(os.environ, GCM_INTERACTIVE='never')
    out = subprocess.run(['git', 'credential', 'fill'], input='protocol=https\nhost=github.com\nusername=%s\n\n' % OWNER,
                         capture_output=True, text=True, env=env).stdout
    for line in out.splitlines():
        if line.startswith('password='):
            return line[len('password='):]
    sys.exit('no token from the credential helper')


T = token()


def api(method, url, data=None, raw=None, content_type=None):
    if not url.startswith('http'):
        url = 'https://api.github.com' + url
    body = raw if raw is not None else (json.dumps(data).encode() if data is not None else None)
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header('Authorization', 'token ' + T)
    req.add_header('Accept', 'application/vnd.github+json')
    req.add_header('X-GitHub-Api-Version', '2022-11-28')
    if content_type or data is not None:
        req.add_header('Content-Type', content_type or 'application/json')
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            text = r.read()
            return r.status, (json.loads(text) if text else None), r.headers
    except urllib.error.HTTPError as e:
        text = e.read()
        try:
            return e.code, json.loads(text), e.headers
        except ValueError:
            return e.code, {'message': text[:200].decode(errors='replace')}, e.headers


def repo(name):
    code, r, _ = api('GET', '/repos/%s/%s' % (OWNER, name))
    return r if code == 200 and r.get('full_name') == '%s/%s' % (OWNER, name) else None


def show(r, keys):
    return ', '.join('%s=%s' % (k, r.get(k)) for k in keys) if r else 'none'


cmd = sys.argv[1] if len(sys.argv) > 1 else ''
if cmd == 'check':
    code, me, headers = api('GET', '/user')
    print('user:', me.get('login'), '| scopes:', headers.get('X-OAuth-Scopes'))
    if me.get('login') != OWNER:
        sys.exit('the token is not %s\'s' % OWNER)
    for n in (PRIVATE, PUBLIC):
        print(n + ':', show(repo(n), ('full_name', 'private', 'fork')))
elif cmd == 'describe':
    code, r, _ = api('PATCH', '/repos/%s/%s' % (OWNER, PUBLIC), {'description': DESCRIPTION, 'homepage': ''})
    print(code, show(r, ('full_name', 'description')))
elif cmd == 'release':
    notes, apk = sys.argv[2], sys.argv[3]
    if os.path.basename(apk) != ASSET:
        sys.exit('the APK should be named %s (android/VERSION is %s)' % (ASSET, VERSION))
    code, r, _ = api('POST', '/repos/%s/%s/releases' % (OWNER, PUBLIC),
                     {'tag_name': TAG, 'target_commitish': 'main', 'name': TITLE,
                      'body': open(notes, encoding='utf-8').read(), 'draft': False, 'prerelease': True})
    if code != 201:
        sys.exit('release: %s %s' % (code, r))
    print('release:', r['html_url'], '| id', r['id'])
    upload = r['upload_url'].split('{')[0] + '?name=' + ASSET
    code, a, _ = api('POST', upload, raw=open(apk, 'rb').read(), content_type='application/vnd.android.package-archive')
    print('asset:', code, show(a, ('name', 'size', 'state', 'browser_download_url')))
elif cmd == 'verify':
    print('public:', show(repo(PUBLIC), ('full_name', 'private', 'fork', 'default_branch', 'description')))
    print('private:', show(repo(PRIVATE), ('full_name', 'private')))
    code, c, _ = api('GET', '/repos/%s/%s/commits/main' % (OWNER, PUBLIC))
    if code == 200:
        print('main:', c['sha'][:7], c['commit']['message'].split('\n')[0], '|', c['commit']['author']['date'],
              '| parent', c['parents'][0]['sha'][:7])
    code, rel, _ = api('GET', '/repos/%s/%s/releases/tags/%s' % (OWNER, PUBLIC, TAG))
    if code == 200:
        print('release:', rel['html_url'], '| prerelease', rel['prerelease'], '| assets',
              [(x['name'], x['size'], x['state']) for x in rel['assets']])
    else:
        print('release %s: not there yet' % TAG)
else:
    sys.exit(__doc__)
