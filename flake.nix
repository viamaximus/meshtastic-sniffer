{
  description = "Nix flake for meshtastic-sniffer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = {
    self,
    nixpkgs,
    flake-utils,
  }:
    flake-utils.lib.eachDefaultSystem (system: let
      pkgs = import nixpkgs {inherit system;};
    in {
      packages.default = pkgs.stdenv.mkDerivation {
        pname = "meshtastic-sniffer";
        version = "git-${self.shortRev or "dirty"}";

        src = self;

        nativeBuildInputs = with pkgs; [
          cmake
          pkg-config
        ];

        buildInputs = with pkgs; [
          fftwFloat
          openssl
          zlib
          mosquitto
          zeromq

          # SDR backends
          rtl-sdr
          hackrf
          airspy
          soapysdr
          libbladeRF
          soapybladerf

          # Intentionally omit uhd.
          # uhd is C++ and currently causes a libstdc++/CXXABI link failure here.
        ];

        postPatch = ''
          substituteInPlace CMakeLists.txt \
            --replace-fail "-O3 -march=native -ggdb3 -g3" "-O3 -ggdb3 -g3" \
            --replace-fail "-O3 -march=native" "-O3"

          # Force-disable UHD/USRP auto-detection.
          substituteInPlace CMakeLists.txt \
            --replace-fail "find_library(UHD_LIB uhd)" "set(UHD_LIB \"\")" \
            --replace-fail "find_path(UHD_INC uhd.h)" "set(UHD_INC \"\")"
        '';

        cmakeFlags = [
          "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
        ];

        enableParallelBuilding = true;

        meta = {
          description = "Wideband passive Meshtastic LoRa receiver";
          homepage = "https://github.com/viamaximus/meshtastic-sniffer";
          platforms = pkgs.lib.platforms.linux;
          mainProgram = "meshtastic-sniffer";
        };
      };

      devShells.default = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
          cmake
          pkg-config
          gdb
        ];

        buildInputs = with pkgs; [
          fftwFloat
          openssl
          zlib
          mosquitto
          zeromq

          rtl-sdr
          hackrf
          airspy
          soapysdr
          libbladeRF
          soapybladerf
        ];

        shellHook = ''
          echo "meshtastic-sniffer dev shell"
          echo "Clean rebuild:"
          echo "  rm -rf build"
          echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo"
          echo "  cmake --build build -j$(nproc)"
        '';
      };
    });
}
