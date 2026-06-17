import { useEffect, useState } from 'react'
import { invoke, money } from '../api.js'

// Optional widgets the user can show/hide. Cash position is always shown (the hero, D17).
const WIDGETS = [
  { key: 'net', label: 'Net profit', value: (d) => d.pnl.net, signed: true },
  { key: 'income', label: 'Income', value: (d) => d.pnl.income, filter: 'income' },
  { key: 'expense', label: 'Expenses', value: (d) => d.pnl.expense, filter: 'expense' },
  { key: 'cashflow', label: 'Net cash flow', value: (d) => d.cf.net, signed: true },
]
const PREF_KEY = 'mb.dashboard.hidden'

function loadHidden() {
  try { return new Set(JSON.parse(localStorage.getItem(PREF_KEY) || '[]')) } catch { return new Set() }
}

export default function Dashboard({ go }) {
  const [pnl, setPnl] = useState(null)
  const [bs, setBs] = useState(null)
  const [cf, setCf] = useState(null)
  const [err, setErr] = useState(null)
  const [customizing, setCustomizing] = useState(false)
  const [hidden, setHidden] = useState(loadHidden)

  useEffect(() => {
    Promise.all([invoke('report.pnl'), invoke('report.balance_sheet'), invoke('report.cash_flow')])
      .then(([p, b, c]) => { setPnl(p); setBs(b); setCf(c) })
      .catch((e) => setErr(String(e)))
  }, [])

  function toggle(key) {
    setHidden((prev) => {
      const next = new Set(prev)
      next.has(key) ? next.delete(key) : next.add(key)
      localStorage.setItem(PREF_KEY, JSON.stringify([...next]))
      return next
    })
  }

  if (err) return <p className="neg">{err}</p>
  if (!pnl || !bs || !cf) return <p>Loading…</p>
  const data = { pnl, bs, cf }

  return (
    <>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <h1 style={{ marginBottom: 0 }}>Dashboard</h1>
        <button className="linkbtn" onClick={() => setCustomizing((v) => !v)}>{customizing ? 'Done' : 'Customize'}</button>
      </div>

      {customizing && (
        <div className="card" style={{ margin: '16px 0', padding: 14 }}>
          <div className="muted" style={{ marginBottom: 8 }}>Choose which widgets to show. Cash position is always shown.</div>
          <div style={{ display: 'flex', flexWrap: 'wrap', gap: 10 }}>
            {WIDGETS.map((w) => (
              <label key={w.key} style={{ display: 'inline-flex', alignItems: 'center', gap: 6, margin: 0 }}>
                <input type="checkbox" style={{ width: 'auto', display: 'inline', margin: 0 }}
                  checked={!hidden.has(w.key)} onChange={() => toggle(w.key)} />
                {w.label}
              </label>
            ))}
          </div>
        </div>
      )}

      <div className="cards" style={{ marginTop: 20 }}>
        <div className="card hero">
          <div className="label">Cash position</div>
          <div className="value">{money(bs.assets)}</div>
        </div>
        {WIDGETS.filter((w) => !hidden.has(w.key)).map((w) => {
          const v = w.value(data)
          const clickable = w.filter && go
          return (
            <div className={'card' + (clickable ? ' clickable' : '')} key={w.key}
              onClick={clickable ? () => go('transactions', { filter: w.filter }) : undefined}>
              <div className="label">{w.label}{clickable ? ' ›' : ''}</div>
              <div className={'value ' + (w.signed ? (v >= 0 ? 'pos' : 'neg') : '')}>{money(v)}</div>
            </div>
          )
        })}
      </div>
    </>
  )
}
