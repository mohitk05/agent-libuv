export default async function afterTool({ name, result }) {
  // Long outputs waste context; trim them before the LLM sees them.
  if (result.length > 4000) return result.slice(0, 4000) + `\n…[${name} output truncated]`;
}
