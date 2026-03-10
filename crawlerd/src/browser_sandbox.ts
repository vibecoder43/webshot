import { spawn } from "node:child_process";
import net from "node:net";
import { mkdir, unlink } from "node:fs/promises";
import { fileURLToPath } from "node:url";

import { kBrowserGeometry, kChromiumVerboseLogging, kChromiumVmodule } from "./config.js";

type Options = {
  browserBin: string;
  userDataDir: string;
  proxyUpstreamSocket: string;
  cdpSocket: string;
  netlogPath: string;
  geometry?: string;
};

const kProxyPort = 3128;
const kDevtoolsPort = 9222;

if (isMainModule()) {
  await main();
}

async function main(): Promise<void> {
  const options = parseArgs(process.argv.slice(2));

  await mkdir(options.userDataDir, { recursive: true });
  await unlink(options.cdpSocket).catch(() => undefined);

  const proxyServer = net.createServer((socket) => {
    const upstream = net.createConnection(options.proxyUpstreamSocket);
    socket.pipe(upstream);
    upstream.pipe(socket);
    const closeBoth = () => {
      socket.destroy();
      upstream.destroy();
    };
    socket.on("error", closeBoth);
    upstream.on("error", closeBoth);
  });

  const cdpServer = net.createServer((socket) => {
    const upstream = net.createConnection({
      host: "127.0.0.1",
      port: kDevtoolsPort,
    });
    socket.pipe(upstream);
    upstream.pipe(socket);
    const closeBoth = () => {
      socket.destroy();
      upstream.destroy();
    };
    socket.on("error", closeBoth);
    upstream.on("error", closeBoth);
  });

  await listenTcp(proxyServer, kProxyPort);
  await listenUnix(cdpServer, options.cdpSocket);

  const chromium = spawn(options.browserBin, buildChromiumArgs(options), {
    stdio: ["ignore", "pipe", "pipe"],
  });
  chromium.stdout.pipe(process.stdout);
  chromium.stderr.pipe(process.stderr);

  const shutdown = async (code: number) => {
    await closeServer(proxyServer).catch(() => undefined);
    await closeServer(cdpServer).catch(() => undefined);
    await unlink(options.cdpSocket).catch(() => undefined);
    process.exit(code);
  };

  process.once("SIGTERM", () => {
    chromium.kill("SIGTERM");
  });
  process.once("SIGINT", () => {
    chromium.kill("SIGTERM");
  });

  chromium.once("exit", (code, signal) => {
    if (signal !== null) {
      void shutdown(1);
      return;
    }
    void shutdown(code ?? 1);
  });
}

function parseArgs(argv: string[]): Options {
  const map = new Map<string, string>();
  for (const arg of argv) {
    if (!arg.startsWith("--")) {
      continue;
    }
    const delimiter = arg.indexOf("=");
    if (delimiter === -1) {
      continue;
    }
    map.set(arg.slice(2, delimiter), arg.slice(delimiter + 1));
  }

  return {
    browserBin: requireArg(map, "browser-bin"),
    userDataDir: requireArg(map, "user-data-dir"),
    proxyUpstreamSocket: requireArg(map, "proxy-upstream-socket"),
    cdpSocket: requireArg(map, "cdp-socket"),
    netlogPath: requireArg(map, "netlog-path"),
    geometry: map.get("geometry"),
  };
}

function requireArg(map: Map<string, string>, name: string): string {
  const value = map.get(name);
  if (value === undefined || value.trim() === "") {
    throw new Error(`missing --${name}`);
  }
  return value;
}

export function buildChromiumArgs(options: Options): string[] {
  const geometry = parseGeometry(options.geometry ?? kBrowserGeometry);
  return [
    "--headless=new",
    "--disable-gpu",
    "--disable-gpu-compositing",
    "--disable-gpu-rasterization",
    "--disable-dev-shm-usage",
    "--disable-background-networking",
    "--disable-breakpad",
    "--disable-crash-reporter",
    "--disable-quic",
    "--no-default-browser-check",
    "--no-first-run",
    "--mute-audio",
    "--hide-scrollbars",
    "--no-sandbox",
    "--no-zygote",
    "--ignore-certificate-errors",
    "--use-gl=angle",
    "--use-angle=swiftshader",
    `--user-data-dir=${options.userDataDir}`,
    `--log-net-log=${options.netlogPath}`,
    "--net-log-capture-mode=IncludeSensitive",
    `--proxy-server=http://127.0.0.1:${kProxyPort}`,
    "--proxy-bypass-list=<-loopback>",
    `--remote-debugging-port=${kDevtoolsPort}`,
    `--window-size=${geometry.width},${geometry.height}`,
    ...(kChromiumVerboseLogging ? ["--enable-logging=stderr", "--log-level=0", "--v=1"] : []),
    ...(kChromiumVmodule === undefined || kChromiumVmodule.trim() === ""
      ? []
      : [`--vmodule=${kChromiumVmodule}`]),
    "about:blank",
  ];
}

export function parseGeometry(value: string): { width: number; height: number } {
  if (value.trim() === "") {
    throw new Error("invalid geometry: must be in WIDTHxHEIGHT format");
  }
  const match = value.match(/^(\d+)x(\d+)$/);
  if (match === null) {
    throw new Error("invalid geometry: must be in WIDTHxHEIGHT format");
  }

  const width = Number.parseInt(match[1]!, 10);
  const height = Number.parseInt(match[2]!, 10);
  if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1) {
    throw new Error("invalid geometry: width and height must be positive integers");
  }
  return { width, height };
}

function isMainModule(): boolean {
  return process.argv[1] !== undefined && process.argv[1] === fileURLToPath(import.meta.url);
}

async function listenTcp(server: net.Server, port: number): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, "127.0.0.1", () => {
      server.off("error", reject);
      resolve();
    });
  });
}

async function listenUnix(server: net.Server, socketPath: string): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(socketPath, () => {
      server.off("error", reject);
      resolve();
    });
  });
}

async function closeServer(server: net.Server): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    server.close((error) => {
      if (error) {
        reject(error);
        return;
      }
      resolve();
    });
  });
}
