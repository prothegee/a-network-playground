| Endpoint | GET | HEAD | POST | PUT | DELETE | PATCH | OPTIONS |
|----------|-----|------|------|-----|--------|-------|---------|
| `/zig` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/zig/json` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `/zig/echo` | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ |
| `/zig/*` (dynamic) | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ |
| `/api/status` | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ |
| `/api/time` | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ |
| `/chat/*` (WebSocket) | ✅ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ |
| Static files (`/public/*`) | ✅ | ✅ | ❌ | ❌ | ❌ | ❌ | ✅ |

### Summary:
- **OPTIONS** - All endpoints support it (CORS preflight)
- **GET/HEAD** - All endpoints support it
- **POST/PUT/PATCH** - Only `/zig` and `/zig/json` support these
- **DELETE** - Only `/zig` and `/zig/json` support this
- **WebSocket upgrade** - Only `/chat/*` with GET method

Other methods return **405 Method Not Allowed** for endpoints that don't support them.

