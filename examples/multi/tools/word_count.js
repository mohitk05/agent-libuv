export default {
  name: 'word_count',
  description: 'Count the words in a text file.',
  parameters: {
    type: 'object',
    properties: { path: { type: 'string' } },
    required: ['path'],
  },
  async run({ path }) {
    const text = await agent.readFile(path);
    return { path, words: text.split(/\s+/).filter(Boolean).length };
  },
};
