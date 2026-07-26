// SUCO Grid — a small VS Code extension that surfaces the SUCO distributed
// compiler grid inside the editor: a status-bar readout of workers + cache-hit
// rate polled from the coordinator, a one-click toggle that injects the SUCO
// compiler launcher into CMake builds, and a shortcut to the web dashboard.
//
// No runtime dependencies: the coordinator is polled with Node's built-in http.
import * as vscode from 'vscode';
import * as http from 'http';

interface Stats {
  workers?: Array<{ name?: string; os?: string }>;
  cache_hits?: number;
  cache_misses?: number;
  total_requests?: number;
  active_jobs?: unknown[];
}

let statusItem: vscode.StatusBarItem;
let pollTimer: ReturnType<typeof setInterval> | undefined;

function config() {
  const c = vscode.workspace.getConfiguration('suco');
  return {
    host: c.get<string>('coordinatorHost', '127.0.0.1'),
    port: c.get<number>('coordinatorPort', 9001),
    launcher: c.get<string>('launcher', 'suco-cl++'),
    poll: Math.max(2, c.get<number>('pollSeconds', 5)),
  };
}

function fetchStats(host: string, port: number): Promise<Stats> {
  return new Promise((resolve, reject) => {
    const req = http.get({ host, port, path: '/api/stats', timeout: 2500 }, (res) => {
      let body = '';
      res.setEncoding('utf8');
      res.on('data', (d) => (body += d));
      res.on('end', () => {
        try {
          resolve(JSON.parse(body) as Stats);
        } catch (e) {
          reject(e);
        }
      });
    });
    req.on('error', reject);
    req.on('timeout', () => req.destroy(new Error('timeout')));
  });
}

async function poll(): Promise<void> {
  const { host, port } = config();
  try {
    const s = await fetchStats(host, port);
    const workers = s.workers?.length ?? 0;
    const hits = s.cache_hits ?? 0;
    const misses = s.cache_misses ?? 0;
    const total = hits + misses;
    const rate = total > 0 ? Math.round((hits / total) * 100) : 0;
    const active = s.active_jobs?.length ?? 0;
    statusItem.text = `$(zap) SUCO: ${workers}w · cache ${rate}%` + (active ? ` · ${active} active` : '');
    statusItem.tooltip = `SUCO grid @ ${host}:${port}\n${workers} workers · cache ${hits}/${total} (${rate}%)` +
      (active ? `\n${active} active job(s)` : '') + `\nClick for actions`;
    statusItem.backgroundColor = undefined;
  } catch {
    statusItem.text = `$(zap) SUCO: offline`;
    statusItem.tooltip = `Cannot reach SUCO coordinator at ${host}:${port}.\nClick to configure or open the dashboard.`;
    statusItem.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
  }
}

function startPolling(): void {
  if (pollTimer) {
    clearInterval(pollTimer);
  }
  void poll();
  pollTimer = setInterval(() => void poll(), config().poll * 1000);
}

// Inject or remove CMAKE_C(XX)_COMPILER_LAUNCHER=<launcher> in the workspace's
// CMake Tools settings. A configure re-run is needed for it to take effect.
async function toggleGrid(): Promise<void> {
  const { launcher } = config();
  const cmake = vscode.workspace.getConfiguration('cmake');
  const key = 'configureSettings';
  const current: Record<string, unknown> = { ...(cmake.get<Record<string, unknown>>(key) ?? {}) };
  const enabled = current['CMAKE_CXX_COMPILER_LAUNCHER'] === launcher;

  if (enabled) {
    delete current['CMAKE_CXX_COMPILER_LAUNCHER'];
    delete current['CMAKE_C_COMPILER_LAUNCHER'];
    await cmake.update(key, current, vscode.ConfigurationTarget.Workspace);
    vscode.window.showInformationMessage('SUCO grid launcher removed. Re-run CMake: Configure for it to take effect.');
  } else {
    current['CMAKE_CXX_COMPILER_LAUNCHER'] = launcher;
    current['CMAKE_C_COMPILER_LAUNCHER'] = launcher;
    await cmake.update(key, current, vscode.ConfigurationTarget.Workspace);
    vscode.window.showInformationMessage(`SUCO grid enabled (${launcher}). Re-run CMake: Configure for it to take effect.`);
  }
}

function openDashboard(): void {
  const { host, port } = config();
  void vscode.env.openExternal(vscode.Uri.parse(`http://${host}:${port}/`));
}

export function activate(context: vscode.ExtensionContext): void {
  statusItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
  statusItem.command = 'suco.menu';
  context.subscriptions.push(statusItem);
  statusItem.show();

  context.subscriptions.push(
    vscode.commands.registerCommand('suco.toggleGrid', toggleGrid),
    vscode.commands.registerCommand('suco.openDashboard', openDashboard),
    vscode.commands.registerCommand('suco.refresh', () => void poll()),
    vscode.commands.registerCommand('suco.menu', async () => {
      const pick = await vscode.window.showQuickPick(
        [
          { label: '$(server) Toggle SUCO grid for CMake builds', cmd: 'suco.toggleGrid' },
          { label: '$(dashboard) Open SUCO dashboard', cmd: 'suco.openDashboard' },
          { label: '$(sync) Refresh status', cmd: 'suco.refresh' },
        ],
        { placeHolder: 'SUCO grid' },
      );
      if (pick) {
        void vscode.commands.executeCommand(pick.cmd);
      }
    }),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration('suco')) {
        startPolling();
      }
    }),
  );

  startPolling();
}

export function deactivate(): void {
  if (pollTimer) {
    clearInterval(pollTimer);
  }
}
