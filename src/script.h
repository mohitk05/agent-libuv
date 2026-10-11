/* Embedded JavaScript (QuickJS) bridge for agent customisation hooks. */
#ifndef SCRIPT_H
#define SCRIPT_H

/* Load an agent script: a single .js file, or a directory of hook files
 * (see README). Returns 0 on success. */
int script_init(const char *path);

int script_has_hook(const char *name);
int script_has_tool(const char *name);

/* JSON array of OpenAI-style tool schemas defined by scripts ("[]" if none). */
const char *script_tools_json(void);
/* JSON object from the script's `config` export ("{}" if none). */
const char *script_config_json(void);

/* Calls hook `name` (or "tool:<name>") with args_json, a JSON array of
 * arguments. Promises are awaited. cb(ud, err, json): err is NULL on
 * success; json is the JSON-encoded return value, or NULL for undefined.
 * cb may run synchronously. */
typedef void (*script_cb)(void *ud, const char *err, const char *json);
void script_call(const char *name, const char *args_json, void *ud, script_cb cb);

#endif
