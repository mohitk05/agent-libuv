# agent-libuv

A minimal LLM agent whose loop *is* the libuv event loop. The LLM and the tools are just async I/O:

| Agent step | libuv mechanism |
|---|---|
| read user prompt | `uv_tty_t` / `uv_pipe_t` on stdin |
| LLM inference (HTTPS) | libcurl multi-socket API driven by `uv_poll_t` + `uv_timer_t` |
| `shell` tool | `uv_spawn` with an async stdout pipe and a kill timer |
| `read_file` tool | `uv_queue_work` (threadpool) |
| tool results -> LLM -> answer | callbacks re-entering the loop |

Works with any OpenAI-compatible `/chat/completions` endpoint.

## Build

Requires a C compiler, CMake ≥ 3.14 and libcurl (dev headers). libuv and json-c are used from the
system if found, otherwise downloaded and built automatically.

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

## Status

Tested on macOS; Linux is covered by CI build only. Windows is untested (uses `cmd.exe` for the shell tool).
Not yet implemented: streaming, scripting hooks (JS/Python), history trimming.
