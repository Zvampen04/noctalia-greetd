{
  description = "Native Noctalia-style greetd greeter";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    noctalia.url = "github:Zvampen04/noctalia";
  };

  outputs =
    {
      self,
      nixpkgs,
      noctalia,
    }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems =
        function: nixpkgs.lib.genAttrs systems (system: function nixpkgs.legacyPackages.${system});
      packageFor =
        pkgs:
        pkgs.callPackage ./nix/package.nix {
          noctaliaPackage = noctalia.packages.${pkgs.stdenv.hostPlatform.system}.default;
        };
    in
    {
      overlays.default = final: _previous: {
        noctalia-greetd = packageFor final;
      };

      packages = forAllSystems (pkgs: rec {
        default = packageFor pkgs;
        noctalia-greetd = default;
      });

      checks = forAllSystems (pkgs: {
        package = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
