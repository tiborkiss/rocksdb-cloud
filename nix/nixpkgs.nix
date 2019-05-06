let
  bootstrap = import <nixpkgs> {};

  wrapped = bootstrap.fetchFromGitHub {
    owner = "NixOS";
    repo = "nixpkgs";
    rev = "343abf7730fb4ea5046a60a553720004e6789d54";
    sha256 = "1l0mxncgmxjc4rcxz5nmmi057gk5v001xnfk49ipafp7y05bajdp";
  };
in
  import wrapped {}
