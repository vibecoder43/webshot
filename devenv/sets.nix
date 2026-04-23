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
    rsync
    drv.boostSml
    boost183
    libarchive
    libarchive.dev
    openssl.dev
    jemalloc
    mold
  ];

  cmakePrefix = with nix; [
    drv.boostSml
    boost183.dev
    fmt.dev
    zstd.dev
    cctz
    yaml-cpp
  ];

  shellRuntime = with nix; [
    bash
    coreutils
    gnused
  ];

  crawlerRuntime = with nix; [
    ungoogled-chromium
    bubblewrap
    socat
  ];

  runtimeTools = shellRuntime ++ crawlerRuntime;

  runtime = with nix;
    [
      postgresql_18
      nginx
      s6
      util-linux
      openssl
      nssTools.tools
    ]
    ++ [drv.seaweedfs];

  systemdRuntime = runtime ++ runtimeTools ++ [drv.pgmigrate drv.repoPy];
  systemdRuntimePath = nix.lib.makeBinPath systemdRuntime;

  sharedLibs = with nix; [
    drv.unialgo
    libarchive
  ];

  userverLibs = drv.userverLibs;
  userver = userverLibs ++ [drv.repoPy];
  buildInputsFor = userverPkg: [userverPkg] ++ sharedLibs ++ userver;
  rpathLibsFor = userverPkg: [userverPkg] ++ sharedLibs ++ userver ++ [nix.stdenv.cc.cc.lib];
  testLibs = userver ++ [nix.libarchive nix.stdenv.cc.cc];
}
