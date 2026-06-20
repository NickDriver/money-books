import { useEffect, useState } from 'react'
import { invoke } from '../api.js'

const fmtDate = (sec) => (sec ? new Date(sec * 1000).toLocaleDateString() : '')

// Start window for multi-company: pick a recent book, create a new company, or open a file by path.
// Opening/creating swaps the active book in the shell, then we reload to re-init against it.
export default function Launcher({ hasCurrent, onCancel }) {
  const [books, setBooks] = useState([])
  const [currentPath, setCurrentPath] = useState(null)
  const [err, setErr] = useState(null)
  const [busy, setBusy] = useState(false)
  const [creating, setCreating] = useState(false)
  const [name, setName] = useState('')
  const [tmpl, setTmpl] = useState('freelancer')
  const [openPath, setOpenPath] = useState('')
  const [confirmPath, setConfirmPath] = useState(null) // book pending "remove from list"
  const [mcp, setMcp] = useState(null) // { book, info } | { book, error } | { book, loading:true }
  const [connecting, setConnecting] = useState(false) // guest: paste a share address
  const [addr, setAddr] = useState('')

  function reloadList() {
    invoke('app.book_list').then((r) => setBooks(r.books || [])).catch((e) => setErr(String(e)))
  }
  useEffect(() => {
    reloadList()
    // ask the shell which book is actually open, so the "open" tag tracks the real current book
    invoke('app.book_current').then((r) => setCurrentPath(r.path || null)).catch(() => {})
  }, [])

  const isCurrent = (b) => (currentPath != null ? b.path === currentPath : !!b.current)

  async function openBook(path) {
    setBusy(true); setErr(null)
    try { await invoke('app.book_open', { path }); window.location.reload() }
    catch (e) { setErr(String(e)); setBusy(false) }
  }
  async function createBook() {
    const n = name.trim()
    if (!n) { setErr('Enter a company name'); return }
    setBusy(true); setErr(null)
    try { await invoke('app.book_create', { name: n, template: tmpl }); window.location.reload() }
    catch (e) { setErr(String(e)); setBusy(false) }
  }
  async function doForget(path, ev) {
    ev.stopPropagation()
    setConfirmPath(null)
    try { await invoke('app.book_forget', { path }); reloadList() }
    catch (e) { setErr(String(e)) }
  }
  async function openMcp(b, ev) {
    ev.stopPropagation()
    setMcp({ book: b, loading: true })
    try { const info = await invoke('app.mcp_info', { path: b.path }); setMcp({ book: b, info }) }
    catch (e) { setMcp({ book: b, error: String(e) }) }
  }
  async function connectShared() {
    const a = addr.trim()
    if (!a) { setErr('Paste the share address you were given'); return }
    setBusy(true); setErr(null)
    try { await invoke('app.share_connect', { address: a }); window.location.reload() }
    catch (e) { setErr(String(e)); setBusy(false) }
  }

  return (
    <div className="wizard">
      <div className="wizard-card" style={{ maxWidth: 720 }}>
        <div className="brand" style={{ padding: 0, marginBottom: 8 }}>Money Books</div>
        <h1 style={{ marginTop: 0 }}>Open a company</h1>
        <p className="muted" style={{ marginTop: 0 }}>Each company is its own book file. Pick one, or create a new one.</p>

        {books.length > 0 && (
          <table style={{ marginBottom: 18 }}>
            <thead><tr><th>Company</th><th>Last opened</th><th></th></tr></thead>
            <tbody>
              {books.map((b) => (
                <tr key={b.path} className={'rowlink' + (isCurrent(b) ? ' current' : '')} onClick={() => !busy && openBook(b.path)}>
                  <td>
                    <div style={{ fontWeight: 600 }}>{b.name}{isCurrent(b) ? <span className="tag st-open" style={{ marginLeft: 8 }}>open</span> : null}</div>
                    <div className="muted" style={{ fontSize: 12 }}>{b.path}</div>
                  </td>
                  <td className="muted">{fmtDate(b.last_opened)}</td>
                  <td className="num">
                    {confirmPath === b.path ? (
                      <span onClick={(e) => e.stopPropagation()}>
                        <span className="muted" style={{ fontSize: 12, marginRight: 6 }}>Remove from list?</span>
                        <button className="linkbtn neg" onClick={(e) => doForget(b.path, e)}>Remove</button>
                        <button className="linkbtn" onClick={(e) => { e.stopPropagation(); setConfirmPath(null) }}>Cancel</button>
                      </span>
                    ) : (
                      <>
                        <button className="linkbtn" title="Show how to connect this company to Claude (MCP)"
                          onClick={(e) => openMcp(b, e)}>MCP</button>
                        <button className="linkbtn" title="Remove from this list (does not delete the file)"
                          style={{ marginLeft: 10 }}
                          onClick={(e) => { e.stopPropagation(); setConfirmPath(b.path) }}>Remove</button>
                      </>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}

        {!creating ? (
          <div style={{ display: 'flex', gap: 8 }}>
            <button className="btn-save filled" onClick={() => setCreating(true)}>+ New company</button>
            <button className="btn-save outline" onClick={() => { setConnecting((v) => !v); setErr(null) }}>⇆ Connect to a shared book</button>
            {hasCurrent && <button className="btn-save outline" onClick={onCancel}>Cancel</button>}
          </div>
        ) : (
          <div className="card" style={{ padding: 16 }}>
            <strong>New company</strong>
            <label>Company name<input value={name} onChange={(e) => setName(e.target.value)} placeholder="e.g. Acme LLC" /></label>
            <label>Starting chart
              <select value={tmpl} onChange={(e) => setTmpl(e.target.value)}>
                <option value="freelancer">Starter template (US freelancer)</option>
                <option value="empty">Start empty</option>
              </select>
            </label>
            <div style={{ display: 'flex', gap: 8, marginTop: 8 }}>
              <button className="btn-save filled" disabled={busy} onClick={createBook}>Create &amp; open</button>
              <button className="btn-save outline" disabled={busy} onClick={() => setCreating(false)}>Cancel</button>
            </div>
          </div>
        )}

        {connecting && (
          <div className="card" style={{ padding: 16, marginTop: 12 }}>
            <strong>Connect to a shared book</strong>
            <p className="muted" style={{ fontSize: 13, margin: '6px 0 8px' }}>
              Paste the address someone shared with you. You’ll get a live, read-only view of their book — view reports and export them.
            </p>
            <textarea rows={3} style={{ width: '100%', resize: 'vertical', fontFamily: 'monospace', fontSize: 12.5 }}
              placeholder="endpoint…" value={addr} onChange={(e) => setAddr(e.target.value)} />
            <div style={{ display: 'flex', gap: 8, marginTop: 8 }}>
              <button className="btn-save filled" disabled={busy || !addr.trim()} onClick={connectShared}>Connect</button>
              <button className="btn-save outline" disabled={busy} onClick={() => { setConnecting(false); setAddr('') }}>Cancel</button>
            </div>
          </div>
        )}

        <details style={{ marginTop: 18 }}>
          <summary className="muted" style={{ cursor: 'pointer' }}>Open an existing book file by path</summary>
          <div style={{ display: 'flex', gap: 8, marginTop: 10, alignItems: 'center' }}>
            <input style={{ flex: 1, marginTop: 0 }} placeholder="/path/to/company.sqlite" value={openPath} onChange={(e) => setOpenPath(e.target.value)} />
            <button className="btn-outline" disabled={busy || !openPath.trim()} onClick={() => openBook(openPath.trim())}>Open</button>
          </div>
        </details>

        {err && <p className="neg">{err}</p>}
      </div>
      {mcp && <McpModal state={mcp} onClose={() => setMcp(null)} />}
    </div>
  )
}

const slug = (s) => 'books-' + String(s || 'company').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '')

// "Connect to Claude" dialog: a ready-to-paste Claude Desktop MCP entry for this one company/book.
function McpModal({ state, onClose }) {
  const { book, info, error, loading } = state
  const [copied, setCopied] = useState(false)
  const [showTools, setShowTools] = useState(false)
  const name = slug(book.name)
  const entry = info
    ? `"${name}": {\n  "command": "${info.command}",\n  "args": ["${info.book_path}"]\n}`
    : ''

  function copy() {
    const done = () => { setCopied(true); setTimeout(() => setCopied(false), 1600) }
    if (navigator.clipboard?.writeText) {
      navigator.clipboard.writeText(entry).then(done).catch(fallback)
    } else { fallback() }
    function fallback() {
      const ta = document.createElement('textarea'); ta.value = entry
      document.body.appendChild(ta); ta.select()
      try { document.execCommand('copy'); done() } catch { /* user can select manually */ }
      ta.remove()
    }
  }

  return (
    <div onClick={onClose} style={{ position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.45)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 50, padding: 20 }}>
      <div className="card" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 660, width: '100%', maxHeight: '85vh', overflow: 'auto', padding: 22 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: 4 }}>
          <h2 style={{ margin: 0 }}>Connect “{book.name}” to Claude</h2>
          <button className="btn-outline" onClick={onClose}>Close</button>
        </div>
        <p className="muted" style={{ marginTop: 6 }}>
          Each company is one MCP server entry. Add this to Claude Desktop and ask it about <strong>{book.name}</strong>’s books — it uses the same vetted operations the app does.
        </p>
        <button className="btn-outline" onClick={() => setShowTools(true)} style={{ marginBottom: 14 }}>🔧 View the tools this exposes</button>

        {loading && <p>Loading…</p>}
        {error && <p className="neg">{error}</p>}
        {info && (
          <>
            <ol style={{ paddingLeft: 18, lineHeight: 1.6 }}>
              <li>Open your Claude Desktop config:<br />
                <code style={{ wordBreak: 'break-all' }}>{info.config_path}</code></li>
              <li>Paste this entry inside the <code>"mcpServers"</code> object (add a comma if it isn’t the last entry):</li>
            </ol>
            <div style={{ display: 'flex', justifyContent: 'flex-end', marginBottom: 6 }}>
              <button className="btn-save filled" onClick={copy}>{copied ? 'Copied ✓' : 'Copy snippet'}</button>
            </div>
            <pre style={{ background: 'var(--code-bg, #0f1729)', color: 'var(--code-fg, #e6edf3)', padding: 14, borderRadius: 8, overflow: 'auto', fontSize: 12.5, margin: '0 0 10px' }}>{entry}</pre>
            <ol start={3} style={{ paddingLeft: 18, lineHeight: 1.6 }}>
              <li>Fully quit Claude Desktop (<strong>⌘Q</strong>, not just close the window) and reopen it.</li>
              <li>“{name}” appears as a tool source. For multiple companies, repeat for each — one entry per book.</li>
            </ol>
            <p className="muted" style={{ fontSize: 12.5, marginBottom: 0 }}>
              Tip: avoid writing to a company from both the app and Claude at the same moment — use one at a time per book. If you ran <code>make clean</code>, rebuild with <code>make mcp</code> first.
            </p>
          </>
        )}
      </div>
      {showTools && <McpToolsModal onClose={() => setShowTools(false)} />}
    </div>
  )
}

// Dedicated window: the full MCP tool surface, so the user knows what a connected client can do.
function McpToolsModal({ onClose }) {
  const [data, setData] = useState(null)
  const [err, setErr] = useState(null)
  useEffect(() => { invoke('mcp.tools').then(setData).catch((e) => setErr(String(e))) }, [])

  const tools = data?.tools || []
  const reads = tools.filter((t) => !t.is_write)
  const writes = tools.filter((t) => t.is_write)
  const Row = (t) => (
    <div key={t.name} style={{ padding: '7px 0', borderBottom: '1px solid var(--border, #2a2a35)' }}>
      <div>
        <code style={{ fontWeight: 600 }}>{t.name}</code>
        {t.policy === 'BLOCK' && <span className="tag" style={{ marginLeft: 8 }}>blocked (hidden from clients)</span>}
      </div>
      <div className="muted" style={{ fontSize: 13 }}>{t.description}</div>
    </div>
  )

  return (
    <div onClick={onClose} style={{ position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.5)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 60, padding: 20 }}>
      <div className="card" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 700, width: '100%', maxHeight: '88vh', overflow: 'auto', padding: 22 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: 4 }}>
          <h2 style={{ margin: 0 }}>Money Books MCP — tools</h2>
          <button className="btn-outline" onClick={onClose}>Close</button>
        </div>
        <p className="muted" style={{ marginTop: 6 }}>
          These are the operations a connected client (e.g. Claude Desktop) can use{data ? ` — ${data.count} in total` : ''}. Reads run freely; <strong>every write asks for your approval</strong> before it executes.
        </p>
        {err && <p className="neg">{err}</p>}
        {!data && !err && <p>Loading…</p>}
        {data && (
          <>
            <h3 style={{ marginBottom: 2 }}>Read · {reads.length} <span className="muted" style={{ fontWeight: 400, fontSize: 13 }}>(no approval needed)</span></h3>
            {reads.map(Row)}
            <h3 style={{ marginBottom: 2, marginTop: 18 }}>Write · {writes.length} <span className="muted" style={{ fontWeight: 400, fontSize: 13 }}>(each requires your approval)</span></h3>
            {writes.map(Row)}
          </>
        )}
      </div>
    </div>
  )
}
