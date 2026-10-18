#!/usr/bin/env python3
"""Install the signed app on a paired iPhone/iPad (USB or the same network) and launch it.

  xcrun devicectl list devices
  python3 -I ios/install_device.py --device '<identifier or name>'

Then copy the disc image into the app's Documents: Finder, select the device, Files tab, drag
"Harry Potter and the Prisoner of Azkaban (USA).iso" onto HP Azkaban (or use --iso below).
"""
import argparse
from pathlib import Path
import plistlib
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--device', required=True, help='paired device identifier or name from devicectl list devices')
    parser.add_argument('--app', type=Path, default=ROOT / 'build/ios-iphoneos/HPAzkaban.app')
    parser.add_argument('--iso', type=Path, help='also copy this disc image into the app Documents (2.2 GB)')
    parser.add_argument('--no-launch', action='store_true', help='install only')
    args = parser.parse_args()
    app = args.app.resolve()
    if not (app / 'embedded.mobileprovision').is_file():
        parser.error('Sign the app first: python3 -I ios/sign_device.py')
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(app)], check=True)
    bundle = plistlib.loads((app / 'Info.plist').read_bytes())['CFBundleIdentifier']
    subprocess.run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', args.device, str(app)], check=True)
    if args.iso:
        subprocess.run(['xcrun', 'devicectl', 'device', 'copy', 'to', '--device', args.device, '--domain-type',
                        'appDataContainer', '--domain-identifier', bundle, '--source', str(args.iso.resolve()),
                        '--destination', 'Documents/' + args.iso.name], check=True)
    if not args.no_launch:
        subprocess.run(['xcrun', 'devicectl', 'device', 'process', 'launch', '--device', args.device, bundle], check=True)


if __name__ == '__main__':
    main()
