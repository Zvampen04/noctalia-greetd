# Noctalia greetd

A native C++ greetd client styled after the Noctalia lock screen. It reuses a
shared visual layout for the central wallpaper, clock, password panel, password
input, and login button while keeping greetd-specific session controls around
that layout.

## Repository ownership

This repository is the canonical owner of the greeter source, KScreenLocker
protocol binding, Noctalia patch, and Nix package. Host configuration
repositories should consume the flake and add only host session definitions,
wallpaper/palette state publication, and service policy.

Make greeter behavior, visuals, protocol, patch, or package changes here first.
Do not add a local C++ source copy or downstream package patch to a NixOS
configuration; update that configuration's pinned `noctalia-greetd` input after
the change is reviewed and validated here.

The flake's `noctalia` input is deliberately followable. A consuming flake can
make it follow the exact Noctalia revision already used by that system:

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

The inherited Noctalia fixup hook is retargeted to the renamed
`noctalia-greetd` executable. Upstream Meson tests are disabled for the release
derivation because Nix's global auto-features setting would otherwise compile
the complete test suite a second time without running it.

This project is distributed under the MIT license used by Noctalia.
