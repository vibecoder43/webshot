{pkgs}: {
  native = with pkgs; [
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

  runtime = with pkgs; [
    postgresql_18
    nginx
    s6
    seaweedfs
    util-linux
    mitmproxy
    socat
  ];
}
