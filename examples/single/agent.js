// A whole agent in one file: every export is an optional hook.
export const config = {
  // model: 'google/gemma-4-e4b',
  // baseUrl: 'http://127.0.0.1:1234/v1',
  maxSteps: 8,
};

// String or function (may be async); evaluated at the start of every turn.
export const systemPrompt = () =>
  `You are a terse terminal assistant. Today is ${new Date().toDateString()}. ` +
  `Use tools when they help and keep answers short.`;

// Define tools right here. run() may be async and can use the `agent` global.
export const tools = [
  {
    name: 'http_get',
    description: 'Fetch a URL with HTTP GET and return the first 2000 characters of the body.',
    parameters: {
      type: 'object',
      properties: { url: { type: 'string' } },
      required: ['url'],
    },
    async run({ url }) {
      const res = await agent.fetch(url);
      return `HTTP ${res.status}\n` + (await res.text()).slice(0, 2000);
    },
  },
  {
    name: 'now',
    description: 'Current date and time (ISO 8601).',
    run: () => new Date().toISOString(),
  },
];

// Return { block: true, result? } to refuse, { args } to rewrite, { result } to skip running.
export function beforeTool({ name, args }) {
  if (name === 'shell' && /\brm\s+-rf\b/.test(args.command))
    return { block: true, result: 'Refused: destructive command.' };
}

export function onUserInput(text) {
  if (text === 'ping') return { reply: 'pong (answered by a hook, no LLM call)' };
}

export function onTurnEnd({ steps }) {
  console.error(`[turn finished after ${steps} tool round(s)]`);
}
