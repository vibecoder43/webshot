{
  nix,
  drv,
}: rec {
  buildNative = with nix; [
    cmake
    ninja
    ada
    pkg-config
    ccache
    boost183
    libarchive
    libarchive.dev
    openssl.dev
    jemalloc
    mold
  ];

  cmakePrefix = with nix; [
    boost183.dev
    fmt.dev
    zstd.dev
    cctz
    yaml-cpp
  ];

  crawlerRuntime = with nix; [
    ungoogled-chromium
    bubblewrap
    socat
  ];

  runtime = with nix;
    [
      postgresql_18
      nginx
      s6
      util-linux
      socat
      openssl
      nssTools.tools
    ]
    ++ [drv.seaweedfs];

  userverLibs = drv.userverLibs;
  userver = userverLibs ++ [drv.repoPy];
  testLibs = userver ++ [nix.libarchive nix.stdenv.cc.cc];
}
