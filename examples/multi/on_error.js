export default function onError({ stage, message }) {
  console.error(`[hook onError] ${stage}: ${message.split('\n')[0]}`);
}
