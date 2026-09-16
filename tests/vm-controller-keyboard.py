"""End-to-end evdev/PAM regression; run only in the disposable desktop VM.

Requires the infinite-desktop input-devices.py fixture and a graphical-user
command wrapper. Replaces only the disposable VM user's password for this test.
"""
import argparse
import json
from pathlib import Path
import secrets
import socket
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--greeter', required=True)
    parser.add_argument('--user-helper', default='/tmp/canvas-user')
    parser.add_argument('--user', default='Zvampen04')
    parser.add_argument('--primary', default='Virtual-1')
    parser.add_argument('--fixture', default='/run/input-fixture.sock')
    args = parser.parse_args()
    if not Path('/etc/wm-audit-session').is_file():
        raise SystemExit('This test requires the disposable desktop audit VM')
    token = secrets.token_hex(4)
    unit = 'osk-dpad-test-' + token
    password = 'q' * (1 + secrets.randbelow(3)) + 'w'
    subprocess.run(['chpasswd'], input=args.user + ':' + password + '\n', text=True, check=True)

    def user(*cmd, check=True):
        return subprocess.run([args.user_helper, *cmd], text=True, capture_output=True, check=check)

    def send(device, events, wait=.09):
        with socket.socket(socket.AF_UNIX) as sock:
            sock.connect(args.fixture)
            sock.sendall((json.dumps({'device': device, 'batches': [{'events': events, 'wait': wait}]})+'\n').encode())
            assert json.loads(sock.makefile().readline())['ok']

    def key(device, code):
        send(device, [[1, code, 1]])
        send(device, [[1, code, 0]])

    def direction(x=0, y=0, device='dualsense'):
        send(device, [[3, 16, x], [3, 17, y]])
        send(device, [[3, 16, 0], [3, 17, 0]])

    def active():
        return user('systemctl', '--user', 'is-active', unit, check=False).stdout.strip() == 'active'

    user('systemd-run', '--user', '--unit='+unit, '--property=Type=notify',
         '--setenv=LOCKSCREEN_GREETER_PRIMARY_OUTPUT='+args.primary, args.greeter, '--lock')
    assert active(), 'Secure lock did not become ready'
    # Allow hotplug discovery and the controller neutral gate to complete.
    time.sleep(2.5)
    # A wrong password must not finish the locker. The normal hardware keyboard
    # shares input with both controllers, including after failed authentication.
    key('keyboard', 45)  # x
    key('keyboard', 28)  # Enter
    time.sleep(3)
    assert active(), 'A failed password ended the lock service'
    # Clear any retained entry without inspecting or exporting password data.
    send('keyboard', [[1, 29, 1], [1, 30, 1]])
    send('keyboard', [[1, 30, 0], [1, 29, 0]])
    key('keyboard', 14)
    direction(x=1)  # Opens keyboard on secondary; initial selection is q.
    # Cursor movement switches from D-pad selection to pointing. Re-entering
    # D-pad mode must still type exactly once per A/X press.
    send('xbox', [[3, 3, 18000]], .15)
    send('xbox', [[3, 3, 0]], .15)
    direction(x=1)
    for index in range(len(password)-1):
        key('dualsense' if index % 2 == 0 else 'xbox', 304)
    direction(x=1, device='xbox')  # w
    key('xbox', 304)
    # Navigate to Enter in the bottom row, traversing row boundaries.
    for _ in range(3):
        direction(y=1)
    for _ in range(4):
        direction(x=1)
    key('dualsense', 304)
    deadline = time.monotonic() + 12
    while active() and time.monotonic() < deadline:
        time.sleep(.1)
    result = user('systemctl', '--user', 'show', unit, '-p', 'Result', '--value').stdout.strip()
    assert not active() and result == 'success', 'D-pad entry did not authenticate and unlock'
    print('PASS: rejected wrong password; mixed keyboard/DualSense/Xbox input; D-pad-only submit and unlock')


if __name__ == '__main__':
    main()
