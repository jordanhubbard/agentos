import { createServer } from 'node:http';
import { VibeSwapPipeline } from './index.mjs';

const PORT = parseInt(process.env.VIBE_SWAP_PORT ?? '9091');
const pipeline = new VibeSwapPipeline();

createServer(async (req, res) => {
  res.setHeader('Content-Type', 'application/json');
  const url = new URL(req.url, `http://localhost:${PORT}`);

  if (req.method === 'POST' && url.pathname === '/stage') {
    const slot = parseInt(url.searchParams.get('slot') ?? '0');
    const name = url.searchParams.get('name') ?? 'agent';
    const chunks = []; for await (const c of req) chunks.push(c);
    const buf = Buffer.concat(chunks);
    const result = await pipeline.stage(slot, buf, name);
    res.end(JSON.stringify(result));
  } else if (req.method === 'POST' && url.pathname === '/validate') {
    const slot = parseInt(url.searchParams.get('slot') ?? '0');
    res.end(JSON.stringify(pipeline.validate(slot)));
  } else if (req.method === 'POST' && url.pathname === '/commit') {
    const slot = parseInt(url.searchParams.get('slot') ?? '0');
    res.end(JSON.stringify(pipeline.commit(slot)));
  } else if (req.method === 'POST' && url.pathname === '/rollback') {
    const slot = parseInt(url.searchParams.get('slot') ?? '0');
    res.end(JSON.stringify(pipeline.rollback(slot)));
  } else if (req.method === 'GET' && url.pathname === '/status') {
    res.end(JSON.stringify(pipeline.status()));
  } else {
    res.statusCode = 404;
    res.end(JSON.stringify({ error: 'not found' }));
  }
}).listen(PORT, () => console.log(`vibe-swap listening on :${PORT}`));
