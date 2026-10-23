// MediaMesh Flasher download relay (Cloudflare Worker, free tier is plenty).
//
// GitHub release downloads send no CORS headers, so a web page can't fetch
// firmware from them. This worker fetches the file from GitHub and hands it to
// the browser with CORS headers. It only relays release downloads from the
// repos listed below, so it can't be used as an open proxy.
//
// Deploy: dash.cloudflare.com > Workers & Pages > Create > Worker, paste this
// file, Deploy. Then set CONFIG.proxy in flasher/index.html to the worker URL
// (e.g. "https://mediamesh-fw.<your-subdomain>.workers.dev").
// Usage: GET <worker>/?url=<https://github.com/OWNER/REPO/releases/download/...>

const ALLOWED = [
  /^https:\/\/github\.com\/jade-on-mesh\/wadamesh-tak\/releases\/download\/[^?#]+$/,
  /^https:\/\/github\.com\/mikecarper\/MeshCore\/releases\/download\/[^?#]+$/,
  /^https:\/\/github\.com\/jade-on-mesh\/spectra-cascadia\/releases\/download\/[^?#]+$/,
];

const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, HEAD, OPTIONS",
  "Access-Control-Expose-Headers": "Content-Length, Content-Type, ETag",
};

export default {
  async fetch(request) {
    if (request.method === "OPTIONS") return new Response(null, { headers: CORS });
    if (request.method !== "GET" && request.method !== "HEAD") {
      return new Response("Method not allowed", { status: 405, headers: CORS });
    }
    const target = new URL(request.url).searchParams.get("url");
    if (!target || !ALLOWED.some(re => re.test(target))) {
      return new Response("Only firmware release downloads from the allowed repos are relayed.", { status: 403, headers: CORS });
    }
    // Release assets are immutable per tag, so let Cloudflare cache them for a day.
    const upstream = await fetch(target, { redirect: "follow", cf: { cacheEverything: true, cacheTtl: 86400 } });
    const headers = new Headers(CORS);
    for (const h of ["content-type", "content-length", "etag", "last-modified"]) {
      const v = upstream.headers.get(h);
      if (v) headers.set(h, v);
    }
    headers.set("Cache-Control", upstream.ok ? "public, max-age=86400" : "no-store");
    return new Response(upstream.body, { status: upstream.status, headers });
  },
};
