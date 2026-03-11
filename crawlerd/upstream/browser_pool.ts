import { spawn, type ChildProcessByStdio } from "node:child_process";
import { mkdir, mkdtemp, readFile, rm, unlink, writeFile } from "node:fs/promises";
import { accessSync, existsSync } from "node:fs";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import type { Readable } from "node:stream";

import { CdpClient } from "./cdp_client.js";

export type BrowserPoolOptions = {
  browserBin: string;
  geometry?: string;
  proxyServer?: string;
};

export type BrowserLease = {
  browser: BrowserInstance;
  reused: boolean;
};

export type BrowserPoolStats = {
  launched: number;
  idle: number;
};

type ManagedBrowser = {
  browser: BrowserInstance;
  leaseCount: number;
  closing: boolean;
  closedPromise: Promise<void>;
  resolveClosed: () => void;
};

const kProxyServer = "http://127.0.0.1:3128";
const kBwrapBin = "bwrap";

export class BrowserPool {
  readonly browserBin: string;
  readonly geometry?: string;
  readonly proxyServer: string;
  readonly browsers = new Map<BrowserInstance, ManagedBrowser>();
  launched = 0;
  closing = false;
  readonly launchPromises = new Set<Promise<ManagedBrowser>>();

  constructor(options: BrowserPoolOptions) {
    this.browserBin = options.browserBin;
    this.geometry = options.geometry;
    this.proxyServer = options.proxyServer ?? kProxyServer;
  }

  async isReady(): Promise<boolean> {
    return await canConnectToProxy(this.proxyServer);
  }

  async acquire(): Promise<BrowserLease> {
    if (this.closing) {
      throw new Error("browser pool is closing");
    }

    const managed = await this.launchBrowser();
    managed.leaseCount++;
    return { browser: managed.browser, reused: false };
  }

  async release(browser: BrowserInstance) {
    const managed = this.browsers.get(browser);
    if (managed === undefined) {
      return;
    }

    if (managed.leaseCount <= 0) {
      throw new Error("release called without an active browser lease");
    }

    managed.leaseCount--;
    if (managed.leaseCount > 0) {
      return;
    }
    await this.closeBrowser(managed);
  }

  async discard(browser: BrowserInstance) {
    const managed = this.browsers.get(browser);
    if (managed === undefined) {
      return;
    }

    if (managed.leaseCount <= 0) {
      throw new Error("discard called without an active browser lease");
    }

    managed.leaseCount--;
    if (managed.leaseCount === 0) {
      await this.closeBrowser(managed);
    }
  }

  async close() {
    this.closing = true;
    await Promise.allSettled(Array.from(this.launchPromises));
    const browsers = Array.from(this.browsers.values());
    await Promise.allSettled(browsers.map(async (browser) => this.closeBrowser(browser)));
  }

  getStats(): BrowserPoolStats {
    return {
      launched: this.launched,
      idle: 0,
    };
  }

  private async launchBrowser(): Promise<ManagedBrowser> {
    const launchPromise = BrowserInstance.launch({
      browserBin: this.browserBin,
      geometry: this.geometry,
      proxyServer: this.proxyServer,
    }).then((browser) => {
      const managed = createManagedBrowser(browser);
      this.browsers.set(browser, managed);
      this.launched++;
      return managed;
    });
    this.launchPromises.add(launchPromise);
    try {
      return await launchPromise;
    } finally {
      this.launchPromises.delete(launchPromise);
    }
  }

  private async closeBrowser(managed: ManagedBrowser): Promise<void> {
    if (managed.closing) {
      await managed.closedPromise;
      return;
    }

    managed.closing = true;
    this.browsers.delete(managed.browser);
    try {
      await managed.browser.close();
    } finally {
      managed.resolveClosed();
    }
  }
}

type LaunchOptions = {
  browserBin: string;
  geometry?: string;
  proxyServer: string;
};

type BrowserPaths = {
  rootDir: string;
  userDataDir: string;
  xdgConfigHome: string;
  xdgCacheHome: string;
  crashpadDir: string;
  proxySocketPath: string;
  cdpSocketPath: string;
  netlogPath: string;
  devNullPath: string;
};

type ProxyEndpoint = {
  host: string;
  port: number;
};

export class BrowserInstance {
  readonly proxyServer: string;
  readonly userDataDir: string;
  readonly cdpSocketPath: string;
  readonly websocketPath: string;
  readonly netlogPath: string;
  readonly proc: ChildProcessByStdio<null, Readable, Readable>;
  readonly cdp: CdpClient;
  readonly proxyBridgeServer: net.Server;
  closed = false;
  stdoutLog = "";
  stderrLog = "";
  finalNetlog = "";

  private constructor(options: {
    proxyServer: string;
    userDataDir: string;
    cdpSocketPath: string;
    websocketPath: string;
    netlogPath: string;
    startupStdout: string;
    startupStderr: string;
    proc: ChildProcessByStdio<null, Readable, Readable>;
    cdp: CdpClient;
    proxyBridgeServer: net.Server;
  }) {
    this.proxyServer = options.proxyServer;
    this.userDataDir = options.userDataDir;
    this.cdpSocketPath = options.cdpSocketPath;
    this.websocketPath = options.websocketPath;
    this.netlogPath = options.netlogPath;
    this.proc = options.proc;
    this.cdp = options.cdp;
    this.proxyBridgeServer = options.proxyBridgeServer;
    this.appendStdout(options.startupStdout);
    this.appendStderr(options.startupStderr);
    this.proc.stdout.on("data", (chunk: Buffer) => {
      this.appendStdout(chunk.toString("utf8"));
    });
    this.proc.stderr.on("data", (chunk: Buffer) => {
      this.appendStderr(chunk.toString("utf8"));
    });
    this.proc.once("exit", () => {
      this.closed = true;
    });
  }

  static async launch(options: LaunchOptions): Promise<BrowserInstance> {
    const paths = await createBrowserPaths();
    const endpoint = parseProxyEndpoint(options.proxyServer);
    const proxyBridgeServer = await createProxyBridgeServer(paths.proxySocketPath, endpoint);
    try {
      const proc = spawnBwrapBrowser(options, paths);
      const browserEndpoint = await waitForBrowserEndpoint(proc);
      const websocketPath = new URL(browserEndpoint.endpoint).pathname;
      const cdp = await CdpClient.connectUnix(paths.cdpSocketPath, websocketPath);
      return new BrowserInstance({
        proxyServer: options.proxyServer,
        userDataDir: paths.userDataDir,
        cdpSocketPath: paths.cdpSocketPath,
        websocketPath,
        netlogPath: paths.netlogPath,
        startupStdout: browserEndpoint.stdout,
        startupStderr: browserEndpoint.stderr,
        proc,
        cdp,
        proxyBridgeServer,
      });
    } catch (error) {
      await closeNetServer(proxyBridgeServer).catch(() => undefined);
      await cleanupPaths(paths.rootDir).catch(() => undefined);
      throw error;
    }
  }

  async isHealthy(): Promise<boolean> {
    if (this.closed || this.proc.killed) {
      return false;
    }

    try {
      await this.cdp.send("Browser.getVersion");
      return true;
    } catch {
      return false;
    }
  }

  async connectCdp(): Promise<CdpClient> {
    if (this.closed || this.proc.killed) {
      throw new Error("browser process is not available for a new CDP connection");
    }
    return await CdpClient.connectUnix(this.cdpSocketPath, this.websocketPath);
  }

  drainLogs(): { stdout: string; stderr: string } {
    const logs = {
      stdout: this.stdoutLog,
      stderr: this.stderrLog,
    };
    this.stdoutLog = "";
    this.stderrLog = "";
    return logs;
  }

  async readNetlog(): Promise<string> {
    try {
      return await readFile(this.netlogPath, "utf8");
    } catch {
      return "";
    }
  }

  async close() {
    if (this.closed) {
      await closeNetServer(this.proxyBridgeServer).catch(() => undefined);
      await cleanupPaths(path.dirname(this.userDataDir)).catch(() => undefined);
      return;
    }

    this.closed = true;
    await this.cdp.close().catch(() => undefined);
    this.proc.kill("SIGTERM");
    await waitForExit(this.proc, 1500).catch(() => {
      this.proc.kill("SIGKILL");
      return waitForExit(this.proc, 1500);
    }).catch(() => undefined);
    this.finalNetlog = await this.readNetlog();
    await closeNetServer(this.proxyBridgeServer).catch(() => undefined);
    await cleanupPaths(path.dirname(this.userDataDir)).catch(() => undefined);
  }

  private appendStdout(text: string) {
    this.stdoutLog = truncateLogBuffer(this.stdoutLog + text);
  }

  private appendStderr(text: string) {
    this.stderrLog = truncateLogBuffer(this.stderrLog + text);
  }
}

function spawnBwrapBrowser(options: LaunchOptions, paths: BrowserPaths): ChildProcessByStdio<null, Readable, Readable> {
  const helperPath = resolveHelperModulePath();
  const helperArgs = [
    ...getHelperExecArgv(helperPath),
    helperPath,
    `--browser-bin=${options.browserBin}`,
    `--user-data-dir=${paths.userDataDir}`,
    `--proxy-upstream-socket=${paths.proxySocketPath}`,
    `--cdp-socket=${paths.cdpSocketPath}`,
    `--netlog-path=${paths.netlogPath}`,
    ...(options.geometry === undefined ? [] : [`--geometry=${options.geometry}`]),
  ];

  const args = [
    "--die-with-parent",
    "--unshare-user",
    "--unshare-net",
    "--unshare-pid",
    "--unshare-ipc",
    "--proc", "/proc",
    "--dev", "/dev",
    "--ro-bind", "/", "/",
    "--tmpfs", "/tmp",
    "--chmod", "1777", "/tmp",
    "--bind", paths.rootDir, paths.rootDir,
    "--bind", paths.devNullPath, "/dev/null",
    "--setenv", "HOME", paths.rootDir,
    "--setenv", "TMPDIR", "/tmp",
    "--setenv", "XDG_CONFIG_HOME", paths.xdgConfigHome,
    "--setenv", "XDG_CACHE_HOME", paths.xdgCacheHome,
    "--setenv", "BREAKPAD_DUMP_LOCATION", paths.crashpadDir,
    "--chdir", paths.rootDir,
    process.execPath,
    ...helperArgs,
  ];

  return spawn(kBwrapBin, args, {
    stdio: ["ignore", "pipe", "pipe"],
  });
}

function resolveHelperModulePath(): string {
  const jsCandidates = [
    fileURLToPath(new URL("../dist/src/browser_sandbox.js", import.meta.url)),
    fileURLToPath(new URL("../src/browser_sandbox.js", import.meta.url)),
  ];
  for (const jsPath of jsCandidates) {
    if (existsSync(jsPath)) {
      return jsPath;
    }
  }
  const tsPath = fileURLToPath(new URL("../src/browser_sandbox.ts", import.meta.url));
  accessSync(tsPath);
  return tsPath;
}

function getHelperExecArgv(helperPath: string): string[] {
  if (!helperPath.endsWith(".ts")) {
    return [];
  }

  const passthroughFlags = new Set([
    "--enable-source-maps",
    "--experimental-loader",
    "--import",
    "--loader",
    "--no-warnings",
    "--require",
    "-r",
  ]);

  const forwarded: string[] = [];
  for (let i = 0; i < process.execArgv.length; i++) {
    const arg = process.execArgv[i]!;
    if (!passthroughFlags.has(arg)) {
      continue;
    }
    forwarded.push(arg);
    if (arg === "--enable-source-maps" || arg === "--no-warnings") {
      continue;
    }
    const value = process.execArgv[i + 1];
    if (value === undefined) {
      break;
    }
    forwarded.push(value);
    i++;
  }
  return forwarded;
}

async function createBrowserPaths(): Promise<BrowserPaths> {
  const rootDir = await mkdtemp(path.join(os.tmpdir(), "crawlerd-browser-"));
  const userDataDir = path.join(rootDir, "profile");
  const xdgConfigHome = path.join(rootDir, "xdg-config");
  const xdgCacheHome = path.join(rootDir, "xdg-cache");
  const crashpadDir = path.join(rootDir, "crashpad");
  const devNullPath = path.join(rootDir, "devnull");
  await mkdir(userDataDir, { recursive: true });
  await mkdir(xdgConfigHome, { recursive: true });
  await mkdir(xdgCacheHome, { recursive: true });
  await mkdir(crashpadDir, { recursive: true });
  await writeFile(devNullPath, "");
  return {
    rootDir,
    userDataDir,
    xdgConfigHome,
    xdgCacheHome,
    crashpadDir,
    proxySocketPath: path.join(rootDir, "proxy.sock"),
    cdpSocketPath: path.join(rootDir, "cdp.sock"),
    netlogPath: path.join(rootDir, "netlog.json"),
    devNullPath,
  };
}

async function createProxyBridgeServer(socketPath: string, endpoint: ProxyEndpoint): Promise<net.Server> {
  await unlink(socketPath).catch(() => undefined);
  const server = net.createServer((socket) => {
    const upstream = net.createConnection({
      host: endpoint.host,
      port: endpoint.port,
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

  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(socketPath, () => {
      server.off("error", reject);
      resolve();
    });
  });
  return server;
}

async function canConnectToProxy(proxyServer: string): Promise<boolean> {
  try {
    const endpoint = parseProxyEndpoint(proxyServer);
    await new Promise<void>((resolve, reject) => {
      const socket = net.createConnection(endpoint);
      const timer = setTimeout(() => {
        socket.destroy();
        reject(new Error("connect timeout"));
      }, 1000);
      socket.once("connect", () => {
        clearTimeout(timer);
        socket.destroy();
        resolve();
      });
      socket.once("error", (error) => {
        clearTimeout(timer);
        reject(error);
      });
    });
    return true;
  } catch {
    return false;
  }
}

function parseProxyEndpoint(proxyServer: string): ProxyEndpoint {
  const url = new URL(proxyServer);
  if (url.protocol !== "http:") {
    throw new Error(`unsupported proxy protocol ${url.protocol}`);
  }
  const port = Number.parseInt(url.port || "80", 10);
  if (!Number.isInteger(port) || port < 1) {
    throw new Error(`invalid proxy port in ${proxyServer}`);
  }
  return {
    host: url.hostname,
    port,
  };
}

async function waitForBrowserEndpoint(
  proc: ChildProcessByStdio<null, Readable, Readable>,
): Promise<{ endpoint: string; stdout: string; stderr: string }> {
  let stderr = "";
  let stdout = "";
  return await new Promise<{ endpoint: string; stdout: string; stderr: string }>((resolve, reject) => {
    const onExit = (code: number | null, signal: NodeJS.Signals | null) => {
      cleanup();
      reject(new Error(
        `chromium exited before exposing devtools (code=${code ?? "null"}, signal=${signal ?? "null"}, stderr=${stderr.trim() || "empty"})`,
      ));
    };
    const onData = (chunk: Buffer) => {
      const text = chunk.toString("utf8");
      stderr += text;
      const match = stderr.match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (match === null) {
        return;
      }
      cleanup();
      resolve({
        endpoint: match[1]!,
        stdout,
        stderr,
      });
    };
    const onStdout = (chunk: Buffer) => {
      stdout += chunk.toString("utf8");
    };
    const timer = setTimeout(() => {
      cleanup();
      reject(new Error(
        `timed out waiting for chromium devtools endpoint (stderr=${stderr.trim() || "empty"}, stdout=${stdout.trim() || "empty"})`,
      ));
    }, 8_000);

    const cleanup = () => {
      clearTimeout(timer);
      proc.off("exit", onExit);
      proc.stderr.off("data", onData);
      proc.stdout.off("data", onStdout);
    };

    proc.once("exit", onExit);
    proc.stderr.on("data", onData);
    proc.stdout.on("data", onStdout);
  });
}

async function waitForExit(
  proc: ChildProcessByStdio<null, Readable, Readable>,
  timeoutMs: number,
): Promise<void> {
  if (proc.exitCode !== null) {
    return;
  }

  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => {
      cleanup();
      reject(new Error("process exit timeout"));
    }, timeoutMs);
    const cleanup = () => {
      clearTimeout(timer);
      proc.off("exit", onExit);
    };
    const onExit = () => {
      cleanup();
      resolve();
    };
    proc.once("exit", onExit);
  });
}

async function closeNetServer(server: net.Server): Promise<void> {
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

async function cleanupPaths(rootDir: string) {
  await rm(rootDir, { recursive: true, force: true }).catch(() => undefined);
}

function truncateLogBuffer(value: string): string {
  const kMaxBytes = 64 * 1024;
  if (value.length <= kMaxBytes) {
    return value;
  }
  return value.slice(value.length - kMaxBytes);
}

function createManagedBrowser(browser: BrowserInstance): ManagedBrowser {
  let resolveClosed!: () => void;
  const closedPromise = new Promise<void>((resolve) => {
    resolveClosed = resolve;
  });
  return {
    browser,
    leaseCount: 0,
    closing: false,
    closedPromise,
    resolveClosed,
  };
}
