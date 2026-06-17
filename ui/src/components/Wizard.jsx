import { useState } from 'react'
import { invoke } from '../api.js'

// First-run choice (D7): seed the US-freelancer starter chart, or start with a bare book.
export default function Wizard({ onDone }) {
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState(null)

  async function choose(template) {
    setBusy(true); setErr(null)
    try {
      await invoke('book.onboard', { template })
      onDone()
    } catch (e) { setErr(String(e)); setBusy(false) }
  }

  return (
    <div className="wizard">
      <div className="wizard-card">
        <div className="brand" style={{ padding: 0, marginBottom: 8 }}>Money Books</div>
        <h1 style={{ marginTop: 0 }}>Welcome — let's set up your books</h1>
        <p className="muted" style={{ marginTop: 0 }}>
          Pick a starting point. You can rename, add, or remove accounts and categories at any time.
        </p>
        <div className="wizard-choices">
          <button className="wizard-choice" disabled={busy} onClick={() => choose('freelancer')}>
            <div className="wc-title">Starter template <span className="tag">recommended</span></div>
            <div className="wc-body">A US freelancer / consulting chart of accounts — checking, income
              categories, common expense categories, and tax accounts. Ready to use immediately.</div>
          </button>
          <button className="wizard-choice" disabled={busy} onClick={() => choose('empty')}>
            <div className="wc-title">Start empty</div>
            <div className="wc-body">Just the few system accounts the engine needs. You'll build your own
              chart of accounts from scratch.</div>
          </button>
        </div>
        {busy && <p className="muted">Setting up…</p>}
        {err && <p className="neg">{err}</p>}
      </div>
    </div>
  )
}
