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
                      <button className="linkbtn" title="Remove from this list (does not delete the file)"
                        onClick={(e) => { e.stopPropagation(); setConfirmPath(b.path) }}>Remove</button>
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

        <details style={{ marginTop: 18 }}>
          <summary className="muted" style={{ cursor: 'pointer' }}>Open an existing book file by path</summary>
          <div style={{ display: 'flex', gap: 8, marginTop: 10, alignItems: 'center' }}>
            <input style={{ flex: 1, marginTop: 0 }} placeholder="/path/to/company.sqlite" value={openPath} onChange={(e) => setOpenPath(e.target.value)} />
            <button className="btn-outline" disabled={busy || !openPath.trim()} onClick={() => openBook(openPath.trim())}>Open</button>
          </div>
        </details>

        {err && <p className="neg">{err}</p>}
      </div>
    </div>
  )
}
