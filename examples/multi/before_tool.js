// Policy hook: block sudo, normalise whitespace in shell commands.
const log = [];

export default async function beforeTool({ name, args }) {
  log.push(name);
  if (name === 'shell' && /\bsudo\b/.test(args.command)) return false; // false = block
  if (name === 'shell') return { args: { command: args.command.trim() } }; // rewrite args
}
