import { serve } from "bun";
import { join } from "node:path";

const root = join(import.meta.dir, "..", "binaries");
const port = Number(process.env.PORT ?? "3000");

serve({
  port,
  fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/libmanual.dylib") {
      const file = Bun.file(join(root, "libmanual.dylib"));
      if (!file.size) {
        return new Response("Artifact not found. Run ./scripts/build.sh first.\n", {
          status: 404,
        });
      }
      return new Response(file, {
        headers: {
          "Content-Type": "application/octet-stream",
          "Content-Disposition": "attachment; filename=\"libmanual.dylib\"",
          "Cache-Control": "no-store",
        },
      });
    }

    return new Response("Not found\n", { status: 404 });
  },
  error(error) {
    console.error("[server] error:", error);
    return new Response("Internal error\n", { status: 500 });
  },
});

console.log(`[server] serving binaries from ${root} on http://localhost:${port}`);
console.log(`[server] GET /libmanual.dylib to download the demo dylib`);
