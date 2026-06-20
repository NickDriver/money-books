import { useEffect, useState } from 'react'
import { invoke } from '../api.js'

// Owner-side "Share this book" panel (Phase 7b). Starts a live, read-only iroh endpoint and
// shows the address the owner sends to a trusted viewer (e.g. an accountant). The viewer pastes
// it into their own Money Books → "Connect to a shared book". Stop ends access to new guests.
export default function ShareModal({ onClose }) {
  const [st, setSt] = useState(null) // { available, sharing, address?, key?, guests? }
  const [err, setErr] = useState(null)
  const [busy, setBusy] = useState(false)
  const [copied, setCopied] = useState(false)

  const refresh = () => invoke('app.share_status').then(setSt).catch((e) => setErr(String(e)))
  useEffect(() => { refresh() }, [])
  // While sharing, poll so the guest count stays live.
  useEffect(() => {
    if (!st?.sharing) return
    const t = setInterval(refresh, 2000)
    return () => clearInterval(t)
  }, [st?.sharing])

  async function start() { setBusy(true); setErr(null); try { setSt(await invoke('app.share_start')) } catch (e) { setErr(String(e)) } finally { setBusy(false) } }
  async function stop()  { setBusy(true); setErr(null); try { setSt(await invoke('app.share_stop'))  } catch (e) { setErr(String(e)) } finally { setBusy(false) } }

  function copy() {
    const addr = st?.address || ''
    const done = () => { setCopied(true); setTimeout(() => setCopied(false), 1600) }
    if (navigator.clipboard?.writeText) navigator.clipboard.writeText(addr).then(done).catch(fallback)
    else fallback()
    function fallback() {
      const ta = document.createElement('textarea'); ta.value = addr
      document.body.appendChild(ta); ta.select()
      try { document.execCommand('copy'); done() } catch { /* select manually */ }
      ta.remove()
    }
  }

  const fp = st?.key ? st.key.slice(0, 12) : ''

  return (
    <div onClick={onClose} style={{ position: 'fixed', inset: 0, background: 'rgba(0,0,0,0.45)', display: 'flex', alignItems: 'center', justifyContent: 'center', zIndex: 50, padding: 20 }}>
      <div className="card" onClick={(e) => e.stopPropagation()} style={{ maxWidth: 600, width: '100%', maxHeight: '85vh', overflow: 'auto', padding: 22 }}>
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: 4 }}>
          <h2 style={{ margin: 0 }}>Share this book</h2>
          <button className="btn-outline" onClick={onClose}>Close</button>
        </div>
        <p className="muted" style={{ marginTop: 6 }}>
          Give a trusted person (e.g. your accountant) a live, <strong>read-only</strong> view of this book over an
          end-to-end-encrypted peer-to-peer link. No third party ever sees your data. They can view reports and export
          them; they cannot make changes.
        </p>

        {err && <p className="neg">{err}</p>}
        {!st && !err && <p>Loading…</p>}

        {st && st.available === false && (
          <p className="neg">This build was compiled without sharing support.</p>
        )}

        {st && st.available !== false && !st.sharing && (
          <div style={{ marginTop: 10 }}>
            <button className="btn-save filled" disabled={busy} onClick={start}>Start sharing</button>
            {st.address && <p className="muted" style={{ fontSize: 12.5, marginTop: 10 }}>Paused. Press Start to let viewers reconnect with the same address.</p>}
          </div>
        )}

        {st && st.sharing && (
          <>
            <ol style={{ paddingLeft: 18, lineHeight: 1.6 }}>
              <li>Send this address to the person you’re sharing with (any channel — it’s a public key, not a secret password):</li>
            </ol>
            <div style={{ display: 'flex', justifyContent: 'flex-end', marginBottom: 6 }}>
              <button className="btn-save filled" onClick={copy}>{copied ? 'Copied ✓' : 'Copy address'}</button>
            </div>
            <pre style={{ background: 'var(--code-bg, #0f1729)', color: 'var(--code-fg, #e6edf3)', padding: 14, borderRadius: 8, overflow: 'auto', fontSize: 12.5, margin: '0 0 10px', wordBreak: 'break-all', whiteSpace: 'pre-wrap' }}>{st.address}</pre>
            <ol start={2} style={{ paddingLeft: 18, lineHeight: 1.6 }}>
              <li>In their Money Books, they choose <strong>Connect to a shared book</strong> and paste it.</li>
              <li>Confirm the fingerprint below matches what they see, so you both know the link is genuine.</li>
            </ol>
            <p className="muted" style={{ fontSize: 13 }}>
              Fingerprint <code>{fp}…</code> · {st.guests ? `${st.guests} guest${st.guests === 1 ? '' : 's'} served this session` : 'no guests yet'}
            </p>
            <div style={{ marginTop: 12 }}>
              <button className="btn-outline neg" disabled={busy} onClick={stop}>Stop sharing</button>
              <span className="muted" style={{ fontSize: 12.5, marginLeft: 10 }}>Cuts off anyone currently connected and blocks new viewers. They can no longer pull data.</span>
            </div>
          </>
        )}
      </div>
    </div>
  )
}
