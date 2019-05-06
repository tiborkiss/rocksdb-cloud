let
  pkgs = import ./nixpkgs.nix; 
in 
  pkgs.callPackage ./default.nix {}
