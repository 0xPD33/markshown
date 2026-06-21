{
  description = "markshown, a tiny GPU-accelerated live markdown viewer";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "markshown";
        version = "0.1.0";
        src = ./.;
        buildInputs = [ pkgs.raylib pkgs.md4c ];
        # fc-match is invoked at runtime; wrap so it's always on PATH.
        nativeBuildInputs = [ pkgs.makeWrapper ];
        buildPhase = ''
          cc -O2 -o markshown markshown.c -lraylib -lmd4c -lm
        '';
        installPhase = ''
          mkdir -p $out/bin
          cp markshown $out/bin/
          # fc-match (fonts) + mmdc (optional, mermaid diagrams) resolved at runtime via PATH.
          wrapProgram $out/bin/markshown --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.fontconfig.bin pkgs.mermaid-cli ]}
        '';
      };

      apps.${system}.default = {
        type = "app";
        program = "${self.packages.${system}.default}/bin/markshown";
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = [ pkgs.raylib pkgs.md4c pkgs.fontconfig.bin pkgs.mermaid-cli pkgs.pkg-config ];
      };
    };
}
