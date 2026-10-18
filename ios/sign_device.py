#!/usr/bin/env python3
"""Sign the device .app with an existing local development profile and certificate.

Never creates or changes anything at Apple: it picks a profile already in
~/Library/Developer/Xcode/UserData/Provisioning Profiles that allows the bundle id
(an exact match first, else the team wildcard) and a matching certificate in the keychain.

  make -f Makefile.ios SDK=iphoneos -j4
  python3 -I ios/sign_device.py
"""
import argparse
from datetime import datetime, timezone
import fnmatch
import hashlib
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--app', type=Path, default=ROOT / 'build/ios-iphoneos/HPAzkaban.app')
    args = parser.parse_args()
    app = args.app.resolve()
    info = plistlib.loads((app / 'Info.plist').read_bytes())
    if not (app / info['CFBundleExecutable']).is_file():
        parser.error('Build the device app first: make -f Makefile.ios SDK=iphoneos')
    bundle = info['CFBundleIdentifier']
    identities = subprocess.check_output(['security', 'find-identity', '-v', '-p', 'codesigning'], text=True)
    valid = set(re.findall(r'\b[0-9A-F]{40}\b', identities))
    now = datetime.now(timezone.utc).replace(tzinfo=None)
    candidates = []
    for profile in (Path.home() / 'Library/Developer/Xcode/UserData/Provisioning Profiles').glob('*.mobileprovision'):
        result = subprocess.run(['security', 'cms', '-D', '-i', str(profile)], capture_output=True)
        if result.returncode:
            continue
        data = plistlib.loads(result.stdout)
        ent = data.get('Entitlements', {})
        team = data.get('TeamIdentifier', [''])[0]
        appid = team + '.' + bundle
        pattern = ent.get('application-identifier', '')
        if data.get('ExpirationDate', datetime.min) < now or not ent.get('get-task-allow'):
            continue
        if not fnmatch.fnmatchcase(appid, pattern):
            continue
        usable = {hashlib.sha1(c).hexdigest().upper() for c in data.get('DeveloperCertificates', [])} & valid
        if usable:
            candidates.append((pattern != appid, profile, data, sorted(usable)[0], appid, team))
    if not candidates:
        raise SystemExit('No local development profile and certificate allow ' + bundle +
                         ' (generate the Xcode project with `make -f Makefile.ios xcode` and sign there)')
    _, profile, data, identity, appid, team = sorted(candidates, key=lambda c: c[0])[0]
    shutil.copy2(profile, app / 'embedded.mobileprovision')
    entitlements = {'application-identifier': appid, 'com.apple.developer.team-identifier': team, 'get-task-allow': True}
    with tempfile.TemporaryDirectory(prefix='hp-sign-') as temp:
        path = Path(temp) / 'entitlements.plist'
        path.write_bytes(plistlib.dumps(entitlements))
        subprocess.run(['codesign', '--force', '--sign', identity, '--entitlements', str(path), '--timestamp=none', str(app)],
                       check=True)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    print('Signed', app.name, 'with profile:', data['Name'])


if __name__ == '__main__':
    main()
