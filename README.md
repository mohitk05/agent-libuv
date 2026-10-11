# agent-libuv

A minimal LLM agent whose loop *is* the libuv event loop. The LLM and the tools are just async I/O:

| Agent step | libuv mechanism |
|---|---|
| read user prompt | `uv_tty_t` / `uv_pipe_t` on stdin |
| LLM inference (HTTPS) | libcurl multi-socket API driven by `uv_poll_t` + `uv_timer_t` |
| `shell` tool | `uv_spawn` with an async stdout pipe and a kill timer |
| `read_file` tool | `uv_queue_work` (threadpool) |
| tool results -> LLM -> answer | callbacks re-entering the loop |
| your customisation | JavaScript hooks (QuickJS), with promises settled from the loop |

Works with any OpenAI-compatible `/chat/completions` endpoint.

## Build

Requires a C compiler, CMake ≥ 3.14, git and libcurl (dev headers). libuv and json-c are used from the
system if found, otherwise downloaded and built automatically; QuickJS-NG is always downloaded.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

(Linux: `apt install libcurl4-openssl-dev`; macOS: nothing extra.)

## Run

```sh
export OPENAI_API_KEY=sk-...
# optional: OPENAI_BASE_URL (default https://api.openai.com/v1), AGENT_MODEL (default gpt-4o-mini)
./build/agent
```

Type a prompt; `/exit` or Ctrl-D quits. Piped input works too.
For Ollama: `OPENAI_BASE_URL=http://localhost:11434/v1 OPENAI_API_KEY=x AGENT_MODEL=llama3.1 ./build/agent`.

**Warning:** the `shell` tool runs model-generated commands with your privileges and no confirmation.

## Writing an agent in JavaScript

The C core is a fixed loop; you customise it with JavaScript (embedded QuickJS-NG, ES modules,
`async`/`await` supported). Pass a script on the command line, set `AGENT_SCRIPT`, or just put an
`agent.js` / `agent/` in the working directory:

```sh
./build/agent examples/single/agent.js   # one file, exports are hooks
./build/agent examples/multi             # a directory, one file per hook
```

### Hooks

Every hook is optional, may be `async`, and receives/returns plain JSON-able values.
Returning `undefined` means "no change".

| Hook | Called | Arguments | Return |
|---|---|---|---|
| `systemPrompt` | start of each turn (a string export also works) | — | `string` replaces the system prompt |
| `onUserInput` | a line was typed | `text` | `string` rewrites it; `null`/`false` swallows it; `{reply}` answers without the LLM |
| `beforeLLM` | before each LLM request | `{model, messages, tools}` | modified request object |
| `afterLLM` | after each LLM response | assistant `message` | modified message (e.g. drop/alter `tool_calls`) |
| `tools` | startup | — | array of `{name, description, parameters, run(args)}` (export an array) |
| `beforeTool` | before a tool runs | `{id, name, args}` | `false` or `{block:true}` blocks; `{args}` rewrites; `{result}` skips running |
| `afterTool` | after a tool runs | `{id, name, args, result}` | `string` (or `{result}`) replaces the result |
| `onError` | any hook, tool or LLM failure | `{stage, message}` | ignored |
| `onTurnEnd` | after the final answer | `{steps, reply}` | ignored |

A `config` export sets `{ model, baseUrl, maxSteps, builtins }` (env vars `AGENT_MODEL` and
`OPENAI_BASE_URL` take precedence). Set `builtins: false` to drop the built-in `shell`/`read_file`
tools; a script tool with the same name overrides a built-in.

### Layouts

- **Single file:** `agent.js` exporting any of the hooks above (named exports, or one default object).
- **Directory:** top-level files named after hooks (`system_prompt.js`, `before-tool.js`,
  `beforeLLM.js`… any case/separator), each with a default export (or the named export). `config.js`
  for config, and `tools/*.js` with one tool per file (default export; the name defaults to the filename).
  An optional `agent.js` in the directory can still export hooks. Files can `import` helpers.

### The `agent` global

Everything below runs on libuv, so scripts never block the loop:

```js
await agent.sleep(ms)
await agent.exec('ls', { timeout: 30000 })    // { code, output, timedOut }
await agent.readFile(path); await agent.writeFile(path, text)
const res = await agent.fetch(url, { method, headers, body })  // body objects are sent as JSON
res.status; res.ok; await res.text(); await res.json()
agent.env('HOME')
```

plus `console.log/error`. See [`examples/`](examples) for working agents.

## Status

Tested on macOS with LM Studio; Linux is covered by CI build only. Windows is untested (uses `cmd.exe`
for the shell tool). Hooks run in a single JS context with no sandboxing. Not yet implemented:
streaming, history trimming, Python scripting.
