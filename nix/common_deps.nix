{pkgs}: {
  native = with pkgs; [
    cmake
    ninja
    ada
    pkg-config
    ccache
    boost183
    openssl.dev
    jemalloc
    mold
  ];

  runtime = with pkgs; [
    podman
    podman-compose
    postgresql_18
  ];
}
