{
  pkgs,
  src,
  toolchain,
  userverBuildPython,
  userverLibs,
}: let
  baseCmakeFlags = [
    "-DUSERVER_DOWNLOAD_PACKAGES=OFF"
    "-DUSERVER_USE_STATIC_LIBS=OFF"
    "-DUSERVER_CHECK_PACKAGE_VERSIONS=0"
    "-DUSERVER_DEBUG_INFO_COMPRESSION=z"
    "-DUSERVER_FEATURE_POSTGRESQL=ON"
    "-DUSERVER_FEATURE_S3API=ON"
    "-DUSERVER_FEATURE_PATCH_LIBPQ=OFF"

    "-DUSERVER_FEATURE_TESTSUITE=ON"
    "-DUSERVER_TESTSUITE_USE_VENV=OFF"

    "-DUSERVER_FEATURE_MONGODB=OFF"
    "-DUSERVER_FEATURE_REDIS=OFF"
    "-DUSERVER_FEATURE_GRPC=OFF"
    "-DUSERVER_FEATURE_CLICKHOUSE=OFF"
    "-DUSERVER_FEATURE_KAFKA=OFF"
    "-DUSERVER_FEATURE_RABBITMQ=OFF"
    "-DUSERVER_FEATURE_MYSQL=OFF"
    "-DUSERVER_FEATURE_ROCKS=OFF"
    "-DUSERVER_FEATURE_YDB=OFF"
    "-DUSERVER_FEATURE_OTLP=OFF"
    "-DUSERVER_FEATURE_SQLITE=OFF"
    "-DUSERVER_FEATURE_ODBC=OFF"
  ];

  mkUserver = {
    buildType,
    sanitize ? "",
  }:
    toolchain.stdenv.mkDerivation {
      name = "userver";

      inherit src;

      patches = [
        ../../patch/userver_testsuite_no_venv.patch
        ../../patch/userver_chaotic_no_venv.patch
        ../../patch/userver_sql_no_venv.patch
        ../../patch/userver_openssl_imported_targets.patch
        ../../patch/userver_cctz_cmake_version.patch
        ../../patch/userver_stdlib_cxx17_variant.patch
        ../../patch/userver_date_std_ws.patch
      ];

      nativeBuildInputs =
        (with pkgs; [
          cmake
          ninja
          python3
          pkg-config
        ])
        ++ [toolchain.cc];

      buildInputs = userverLibs ++ [userverBuildPython];

      dontStrip = true;

      cmakeFlags =
        baseCmakeFlags
        ++ [
          "-DUSERVER_INSTALL=ON"
          "-DUSERVER_CHAOTIC_USE_VENV=OFF"
          "-DUSERVER_CHAOTIC_PYTHON_BINARY=${userverBuildPython}/bin/python3"
          "-DUSERVER_SQL_USE_VENV=OFF"
          "-DUSERVER_SQL_PYTHON_BINARY=${userverBuildPython}/bin/python3"
          "-DIconv_IS_BUILT_IN=ON"
          "-DCMAKE_BUILD_TYPE=${buildType}"
          "-DOPENSSL_ROOT_DIR=${pkgs.openssl.dev}"
          "-DOPENSSL_INCLUDE_DIR=${pkgs.openssl.dev}/include"
          "-DOPENSSL_CRYPTO_LIBRARY=${pkgs.openssl}/lib/libcrypto.so"
          "-DOPENSSL_SSL_LIBRARY=${pkgs.openssl}/lib/libssl.so"
          "-Dfmt_DIR=${pkgs.fmt.dev}/lib/cmake/fmt"
          "-Dcctz_DIR=${pkgs.cctz}/lib/cmake/cctz"
        ]
        ++ (
          if sanitize == ""
          then []
          else ["-DUSERVER_SANITIZE=${sanitize}"]
        );
      hardeningDisable = ["all"];
    };

  userver-release = mkUserver {
    buildType = "Release";
    sanitize = "";
  };

  userver-debug-addr-ub = mkUserver {
    buildType = "Debug";
    sanitize = "addr;ub";
  };
in {
  inherit userver-release userver-debug-addr-ub;
  default = userver-release;
}
