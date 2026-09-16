# Noctalia greetd

One native C++ screen for greetd login and Wayland session locking. Both modes
use `NativeGreeter`, `lock_visual_layout` and `OnScreenKeyboard`; there is no
separate lock-screen UI. Login uses greetd authentication, while `--lock` uses
PAM and `ext-session-lock-v1`. Session/profile controls are only available before
login.

Every output gets a wallpaper and clock. Only the primary output gets an
authentication field. `LOCKSCREEN_GREETER_PRIMARY_OUTPUT` selects that output,
with a connected-output fallback if it disappears. The keyboard button works
on every display. On a secondary display the keyboard fills the display; on the
primary display the authentication controls move above it.

The MIT-licensed built-in keyboard supports Swedish letters, numbers, symbols,
Shift, cursor keys, backspace and Enter. Touch, physical keyboard, mouse and
controller input can be mixed. DualSense and Xbox right sticks move the normal
Wayland cursor; X/A clicks and Circle/B right-clicks. The D-pad opens/navigates
the keyboard and X/A types the highlighted key. D-pad navigation wraps at the
edges and repeats while held. Enter submits; Hide minimizes the keyboard. Moving the stick or using a
mouse/touch clears key selection. Gamepads must be neutral on connection and
are released when the screen exits.

`--lock` requires the compositor's session-lock protocol and a PAM service named
`noctalia-greetd`. It never falls back to an ordinary fullscreen window. A
systemd `Type=notify` unit receives readiness only after the compositor reports
that the session is locked. Failed authentication and process failure do not
send an unlock request. Do not use `--kscreenlocker` as a standalone Wayland
locker: that compatibility mode depends on KScreenLocker's external lock host.

The compositor must expose multiple outputs and layer shell for login, plus
virtual-pointer support for controller cursor input. Give the greeter read
access only to gamepad event devices, not arbitrary input devices. Password
keys stay in-process and are not passed through a clipboard, command arguments,
network keyboard, prediction service, or persistent key history.

## Repository ownership

This repository owns the greeter source, KScreenLocker protocol binding,
Noctalia patch, and Nix package.

Host configuration repositories should consume the flake. Those repositories
should add only these items:

- Host session definitions.
- Wallpaper, palette and appearance-snapshot publication.
- Service policy.

Make greeter behavior, visual, protocol, patch, or package changes here first.
Do not add a local C++ source copy to a NixOS configuration.
Do not add a downstream package patch to a NixOS configuration.

Review and validate the change in this repository. Then update the consuming
configuration's pinned `noctalia-greetd` input.

The flake permits its `noctalia` input to follow another input. A consuming
flake can use the system's existing Noctalia revision:

```nix
noctalia-greetd = {
  url = "github:Zvampen04/noctalia-greetd";
  inputs.nixpkgs.follows = "nixpkgs";
  inputs.noctalia.follows = "noctalia";
};
```

## Build and validation

```sh
nix build
nix flake check
```

The package redirects the inherited Noctalia fixup hook to the renamed
`noctalia-greetd` executable.

The greeter derivation disables the full shell test suite and registers its own
`greetd_appearance` test against the shared native core. It inherits the package
check phase, which runs this appearance parser and control-state regression.

This project is distributed under the MIT license used by Noctalia.

`tests/vm-controller-keyboard.py` exercises real evdev input and PAM in the full
desktop audit VM. Start the infinite-desktop `input-devices.py` fixture first,
then supply the built greeter with `--greeter` and a graphical-user command
wrapper with `--user-helper`. The test replaces the disposable VM password.
It refuses to run without `/etc/wm-audit-session`. It checks failed authentication,
mixed hardware keyboard/DualSense/Xbox input, cursor-to-D-pad transitions and
successful unlock through the secondary keyboard.

## Desktop keyboard

The same keyboard is available with `noctalia-greetd --keyboard`. It opens a
bottom layer on the selected output and leaves keyboard focus in the current
application. Keys use the Wayland virtual-keyboard protocol; login and lock
passwords still remain inside the authentication process.

Mouse and touch activate keys directly. Infinite Desktop routes D-pad navigation
and Cross/A activation to the private runtime socket while the pointer is over
the keyboard. It remains the controller owner: the desktop keyboard never grabs
controllers. Hide exits this mode. The NixOS launcher entry is **On Screen Keyboard**.

## Appearance transport

The shared scene loads version 1 JSON from
`LOCKSCREEN_GREETER_APPEARANCE_FILE`. It carries the native design metrics,
material parameters, role/family/surface overrides, control variants and their
palette roles, font, palette, motion, and border/radius policies. The owned
Noctalia core supplies the same rendering and control definitions to the shell
and greeter; preset names do not select rendering code.

Noctalia publishes live appearance atomically to
`$XDG_RUNTIME_DIR/noctalia/appearance.json`. Lock and desktop keyboard launchers
watch this path so preview, Save and Cancel changes update existing controls.
Updates preserve passwords, authentication state, focus and surface identities.
Invalid, unsupported or oversized snapshots retain the last valid appearance.

A separate `$XDG_STATE_HOME/noctalia/appearance.json` contains committed effective
configuration. The NixOS PAM refresh reads it as the session user, merges the
latest login palette and atomically publishes the login copy at
`/var/lib/noctalia-greetd/appearance.json`. Unsaved previews never enter that
committed copy. Wallpaper selection and authentication/session configuration
remain separate from the JSON appearance data.

Legacy `LOCKSCREEN_GREETER_SURFACE_MATERIAL`,
`LOCKSCREEN_GREETER_CORNER_SCALE`, `LOCKSCREEN_GREETER_ANIMATIONS` and palette
inputs remain migration defaults until a valid JSON snapshot is loaded.
Login, locking and their keyboards render only their own wallpaper and disable
external material transport. Desktop `--keyboard` can use the session
compositor's material scene transport on its existing non-focus-taking layer.
