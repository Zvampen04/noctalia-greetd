# AGENTS.md

Scope: this repository.

- This repository owns the native Noctalia greetd binary and its shared lock-screen layout.
- This repository also owns the protocol binding, patch, and Nix package.
- Implement greeter behavior, visuals, protocol, and package changes here.
- Downstream NixOS repositories may provide host session and state integration.
- Downstream repositories must not vendor or patch a second greeter source tree.
- Keep the central lock-screen visuals in `lock_visual_layout`.
- Add greetd-only controls around that shared layout.
- Preserve source compatibility across Noctalia's supported core/input and JSON
  header layouts.
- Run `nix flake check` for every source or package change.

- Login and session locking must use the same NativeGreeter scene and keyboard.
- Only the surface role and authentication backend differ in `--lock` mode.
- Never fall back from ext-session-lock to a toplevel or layer-shell locker.
- Notify lock readiness only after the compositor's locked event. Only successful PAM authentication can request unlock.
- Keep controller input scoped to this screen and release its gamepad grabs before returning to the session.

- `--keyboard` reuses OnScreenKeyboard on a non-focus-taking desktop layer, with a desktop-only virtual-keyboard transport. It must not authenticate, lock the session or grab gamepads. Login/lock keystrokes remain in process.
- Desktop controller navigation uses the private user-runtime datagram socket; the compositor remains the controller owner.

- Build against the owned Noctalia core shared with the desktop shell. Keep
  appearance rendering driven by parameters and semantic scopes, not preset names.
- `LOCKSCREEN_GREETER_APPEARANCE_FILE` selects the versioned JSON appearance
  snapshot. `appearance.h` validates and watches it, preserving the last valid
  snapshot and existing authentication, focus, input and surface state on updates.
- Login receives a separately published committed snapshot. Lock and desktop OSK
  may consume the live user-runtime snapshot, including unsaved previews.
- Keep material parameters, control variants, metrics, scope overrides, font,
  palette, border/radius policy and motion in this appearance-only transport.
  Never derive authentication or session policy from appearance settings.
- Retain legacy material, corner-scale, motion and palette environment inputs as
  migration defaults when no valid JSON snapshot is available.
- Login, locking and their keyboards disable external material transport and
  sample only their own wallpaper. Desktop `--keyboard` may publish its material
  scene to the session compositor while retaining its non-focus-taking layer.

- `lock_widget_services.h` owns one process-local monitor, PipeWire connection and
  spectrum analyzer shared by output scenes. Never connect to a selected user's
  services from login. Missing services render unavailable state without blocking
  authentication. Destroy widget scenes before their services.
- `lock_widgets_scene.h` admits only explicit first-party passive widget types to
  the shared desktop factory. Keep calendar events, scripts, personal stores and
  authentication controls out of this path. Schedule visual frames independently
  from one-second data refreshes; a frame request alone must not repaint outputs.
