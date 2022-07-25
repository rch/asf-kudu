
let
    sources = import ./nix/sources.nix;
    pkgs = import sources.nixpkgs {};
    frameworks = pkgs.darwin.apple_sdk.frameworks;
in pkgs.stdenv.mkDerivation {
  name = "kudu-env";
  buildInputs = [
    pkgs.autoconf
    pkgs.automake
    pkgs.cmake
    pkgs.curl
    pkgs.cyrus_sasl
    pkgs.flex
    pkgs.gcc
    pkgs.gdb
    pkgs.git
    pkgs.glog
    pkgs.kerberos
    pkgs.libtool
    #lsb-release
    pkgs.openssl
    pkgs.patch
    pkgs.perl
    pkgs.pkg-config
    pkgs.python
    #pkgs.rsync
    pkgs.sysctl
    pkgs.unzip
    pkgs.vim
    pkgs.which
    #numactl
    frameworks.Security
    frameworks.CoreFoundation
    frameworks.CoreServices
  ];
  shellHook = ''
    export PS1="[$name] \[$txtgrn\]\u@\h\[$txtwht\]:\[$bldpur\]\w \[$txtcyn\]\$git_branch\[$txtred\]\$git_dirty \[$bldylw\]\$aws_env\[$txtrst\]\$ "
    export NIX_LDFLAGS="-F${frameworks.CoreFoundation}/Library/Frameworks -framework CoreFoundation $NIX_LDFLAGS";
    NO_REBUILD_THIRDPARTY=1
  '';
}

