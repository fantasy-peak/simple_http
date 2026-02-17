{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs =
    inputs@{ nixpkgs, flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = nixpkgs.lib.systems.flakeExposed;

      perSystem =
        { config, pkgs, ... }:
        {
          devShells.default = pkgs.mkShell {
            name = "simple_http";

            packages = with pkgs; [
              gcc
              xmake
              unzip
              gnum4
              pkg-config
              cmake
              ninja
              # for openssl package
              perl
              # mostly for clangd, clang-format and clang-tidy
              clang-tools
            ];
          };
        };
    };
}
