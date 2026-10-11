export default async function systemPrompt() {
  const { output } = await agent.exec('uname -sm');
  return `You are a helpful terminal agent running on ${output.trim()}. Be concise.`;
}
