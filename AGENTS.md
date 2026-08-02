# AGENTS.md

Scope: this repository.

- This repository is the sole source owner for the native Noctalia greetd
  binary, its shared lockscreen visual layout, protocol binding, patch, and Nix
  package.
- Implement greeter behavior, visuals, protocol, and package changes here.
  Downstream NixOS repositories may provide host session/state integration but
  must not vendor or patch a second greeter source tree.
- Keep the central lockscreen visuals shared through `lock_visual_layout`; add
  greetd-only controls around that shared layout.
- Preserve source compatibility across Noctalia's supported core/input and JSON
  header layouts.
- Run `nix flake check` for every source or package change.
