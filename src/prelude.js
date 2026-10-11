// Evaluated once at startup. Sets up the hook registry and the `agent` global.
(() => {
  const HOOKS = [
    'systemPrompt', 'onUserInput', 'beforeLLM', 'afterLLM',
    'beforeTool', 'afterTool', 'onError', 'onTurnEnd',
  ];
  const norm = (s) => s.toLowerCase().replace(/[-_.]/g, '');
  const hooks = {};
  const tools = new Map();
  let config = {};

  const addTool = (t, fallbackName, file) => {
    if (Array.isArray(t)) return t.forEach((x) => addTool(x, undefined, file));
    if (typeof t === 'function') t = { run: t };
    t = { ...t };
    t.name ??= fallbackName;
    if (!t.name || typeof t.run !== 'function')
      throw new Error(`invalid tool in ${file}: needs a name and a run() function`);
    tools.set(t.name, t);
  };

  // Called once per script file with its module namespace.
  globalThis.__register = (file, ns) => {
    const parts = file.replace(/\\/g, '/').split('/');
    const base = parts.pop().replace(/\.[mc]?js$/, '');
    const def = ns.default;

    if (parts.at(-1) === 'tools') return addTool(def ?? ns.tool ?? ns.tools, base, file);

    if (norm(base) === 'config') {
      config = { ...config, ...(def ?? ns.config ?? {}) };
      return;
    }
    const hook = HOOKS.find((h) => norm(h) === norm(base));
    if (hook) {
      const fn = def ?? ns[hook];
      if (fn !== undefined) hooks[hook] = fn;
      return;
    }
    // Any other file (e.g. agent.js): its exports are hooks.
    const src = { ...ns, ...(def && typeof def === 'object' ? def : {}) };
    for (const h of HOOKS) if (src[h] !== undefined) hooks[h] = src[h];
    if (src.tools) addTool(src.tools, undefined, file);
    if (src.config) config = { ...config, ...src.config };
  };

  globalThis.__call = async (name, argsJson) => {
    // Errors are returned (prefixed with \x01) rather than thrown so that the host
    // can always attach its handler before anything is reported as unhandled.
    try {
      const args = JSON.parse(argsJson);
      let r;
      if (name.startsWith('tool:')) {
        r = await tools.get(name.slice(5)).run(...args);
      } else {
        const h = hooks[name];
        r = typeof h === 'function' ? await h(...args) : h;
      }
      return r === undefined ? undefined : JSON.stringify(r);
    } catch (e) {
      return '\x01' + (e && e.stack ? `${e}\n${e.stack}` : String(e));
    }
  };
  globalThis.__hasHook = (n) => hooks[n] !== undefined;
  globalThis.__hasTool = (n) => tools.has(n);
  globalThis.__config = () => JSON.stringify(config);
  globalThis.__tools = () =>
    JSON.stringify(
      [...tools.values()].map((t) => ({
        type: 'function',
        function: {
          name: t.name,
          description: t.description ?? '',
          parameters: t.parameters ?? { type: 'object', properties: {} },
        },
      })),
    );

  const N = globalThis.__native;
  const fmt = (v) => (typeof v === 'string' ? v : v instanceof Error ? v.stack || String(v) : (() => {
    try { return JSON.stringify(v); } catch { return String(v); }
  })());
  globalThis.console = {
    log: (...a) => N.log(0, a.map(fmt).join(' ')),
    info: (...a) => N.log(0, a.map(fmt).join(' ')),
    warn: (...a) => N.log(1, a.map(fmt).join(' ')),
    error: (...a) => N.log(1, a.map(fmt).join(' ')),
  };

  // Async APIs backed by libuv.
  globalThis.agent = {
    sleep: (ms) => N.sleep(ms),
    env: (name) => N.env(name),
    exec: (cmd, opts = {}) => N.exec(cmd, opts.timeout ?? 30000),
    readFile: (path) => N.readFile(path),
    writeFile: (path, data) => N.writeFile(path, String(data)),
    fetch: async (url, o = {}) => {
      const body = o.body === undefined ? '' : typeof o.body === 'string' ? o.body : JSON.stringify(o.body);
      const headers = { ...(o.body !== undefined && typeof o.body !== 'string' ? { 'Content-Type': 'application/json' } : {}), ...(o.headers ?? {}) };
      const r = await N.fetch(o.method ?? (body ? 'POST' : 'GET'), url, JSON.stringify(headers), body, o.timeout ?? 60000);
      return {
        status: r.status,
        ok: r.status >= 200 && r.status < 300,
        text: async () => r.body,
        json: async () => JSON.parse(r.body),
      };
    },
  };
  delete globalThis.__native;
})();
