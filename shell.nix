let
  sources = import ./nix/sources.nix;
  pkgs = import sources.nixpkgs {};
in
with pkgs;
pkgs.mkShell {
  buildInputs = [
    autoconf
    automake
    bash
    cmake
    curl
    cyrus_sasl
    flex
    gdb
    git
    jdk11
    glog
    kerberos
    libtool
    maven
    openssl
    patch
    perl
    pkg-config
    python311
    sysctl
  ];
}

