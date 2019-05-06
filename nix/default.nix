# Custom default for rocksdb-cloud.
{ stdenv, fetchFromGitHub, fixDarwinDylibNames, which, perl, snappy ? null
, zlib ? null, bzip2 ? null, lz4 ? null, numactl ? null, aws-sdk-cpp
, jemalloc ? null, gperftools ? null, enableLite ? false
}:
let
  malloc = if jemalloc != null then jemalloc else gperftools;
in
  stdenv.mkDerivation rec {
    name = "rocksdb-cloud-${version}";
    version = "5.18.3";
    src = ../.;
    nativeBuildInputs = [ which perl ];
    buildInputs = [ aws-sdk-cpp snappy zlib bzip2 lz4 malloc fixDarwinDylibNames ];
    postPatch = ''
      # Hack to fix typos
      sed -i 's,#inlcude,#include,g' build_tools/build_detect_platform
    '';
    # Environment vars used for building certain configurations
    PORTABLE = "1";
    USE_SSE = "1";
    USE_AWS = "1";
    CMAKE_CXX_FLAGS = "-std=gnu++11";
    JEMALLOC_LIB = stdenv.lib.optionalString (malloc == jemalloc) "-ljemalloc";
    ${if enableLite then "LIBNAME" else null} = "librocksdb_lite";
    ${if enableLite then "CXXFLAGS" else null} = "-DROCKSDB_LITE=1";
    buildAndInstallFlags = [
      "USE_RTTI=1"
      "DEBUG_LEVEL=0"
      "DISABLE_WARNING_AS_ERROR=1"
    ];
    buildFlags = buildAndInstallFlags ++ [
      "shared_lib"
      "static_lib"
    ];
    installFlags = buildAndInstallFlags ++ [
      "INSTALL_PATH=\${out}"
      "install-shared"
      "install-static"
    ];
    postInstall = ''
      # Might eventually remove this when we are confident in the build process
      echo "BUILD CONFIGURATION FOR SANITY CHECKING"
      cat make_config.mk
    '';
    enableParallelBuilding = true;
    meta.priority = 0; # Need this in order to avoid nix-env conflics with standard rocksdb
  }
