# Serves this copy of the FM-4 webapp on http://localhost and opens it in the browser.
# Windows only, no Python needed. Web MIDI needs a secure context; localhost counts as one.
# Files are sent with "Cache-Control: no-store", and a busy port is skipped.
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PSScriptRoot)
$types = @{ '.html'='text/html; charset=utf-8'; '.js'='text/javascript; charset=utf-8'; '.mjs'='text/javascript; charset=utf-8';
  '.css'='text/css; charset=utf-8'; '.json'='application/json'; '.wasm'='application/wasm'; '.svg'='image/svg+xml';
  '.png'='image/png'; '.ico'='image/x-icon'; '.md'='text/plain; charset=utf-8'; '.txt'='text/plain; charset=utf-8' }
$listener = $null
foreach ($port in 8041..8059) {
  $l = New-Object Net.HttpListener
  $l.Prefixes.Add("http://localhost:$port/")
  try { $l.Start(); $listener = $l; break } catch { $l.Close() }
}
if (-not $listener) { Write-Host 'No free port between 8041 and 8059.'; Read-Host 'Press Enter to close'; exit 1 }
$url = "http://localhost:$port/"
Write-Host "FM-4 webapp: $url  (close this window to stop)"
try { Start-Process $url } catch { Write-Host "Open $url in Chrome or Edge." }
while ($listener.IsListening) {
  $ctx = $listener.GetContext()
  $res = $ctx.Response
  try {
    $rel = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath).TrimStart('/')
    if ($rel -eq '' -or $rel.EndsWith('/')) { $rel += 'index.html' }
    $path = [IO.Path]::GetFullPath((Join-Path $root $rel))
    if (-not $path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
      $res.StatusCode = 404; $bytes = [Text.Encoding]::UTF8.GetBytes('not found')
    } else {
      $bytes = [IO.File]::ReadAllBytes($path)
      $ext = [IO.Path]::GetExtension($path).ToLowerInvariant()
      $res.ContentType = if ($types.ContainsKey($ext)) { $types[$ext] } else { 'application/octet-stream' }
    }
    $res.Headers['Cache-Control'] = 'no-store'
    $res.ContentLength64 = $bytes.Length
    $res.OutputStream.Write($bytes, 0, $bytes.Length)
  } catch { Write-Host "error: $_" } finally { $res.Close() }
}
