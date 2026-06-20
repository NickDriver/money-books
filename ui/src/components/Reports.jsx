import { useEffect, useState } from 'react'
import { invoke, money, downloadCsv } from '../api.js'

const EXPORTS = [
  ['export.trial_balance', 'Trial Balance'],
  ['export.pnl', 'Profit & Loss'],
  ['export.balance_sheet', 'Balance Sheet'],
  ['export.general_ledger', 'General Ledger'],
  ['export.journal', 'Journal'],
]

export default function Reports() {
  const [pnl, setPnl] = useState(null)
  const [bs, setBs] = useState(null)
  const [err, setErr] = useState(null)
  const [busy, setBusy] = useState(null)
  const [saved, setSaved] = useState(null)

  useEffect(() => {
    Promise.all([invoke('report.pnl'), invoke('report.balance_sheet')])
      .then(([p, b]) => { setPnl(p); setBs(b) })
      .catch((e) => setErr(String(e)))
  }, [])

  async function exportCsv(method) {
    setBusy(method); setSaved(null); setErr(null)
    try {
      const file = await downloadCsv(method)
      setSaved(file)
    } catch (e) {
      setErr(String(e))
    } finally {
      setBusy(null)
    }
  }

  if (err && !pnl) return <p className="neg">{err}</p>
  if (!pnl || !bs) return <p>Loading…</p>

  return (
    <>
      <h1>Reports</h1>
      <div className="card" style={{ marginBottom: 16 }}>
        <div className="label" style={{ marginBottom: 8 }}>Export for your accountant (CSV)</div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
          {EXPORTS.map(([method, label]) => (
            <button key={method} onClick={() => exportCsv(method)} disabled={busy === method}>
              {busy === method ? 'Exporting…' : label}
            </button>
          ))}
        </div>
        {saved && <p className="label" style={{ marginTop: 8 }}>Saved <strong>{saved}</strong></p>}
        {err && pnl && <p className="neg" style={{ marginTop: 8 }}>{err}</p>}
      </div>
      <div className="card" style={{ marginBottom: 16 }}>
        <div className="label" style={{ marginBottom: 8 }}>Profit &amp; Loss</div>
        <table>
          <tbody>
            <tr><td>Income</td><td className="num">{money(pnl.income)}</td></tr>
            <tr><td>Expenses</td><td className="num">{money(pnl.expense)}</td></tr>
            <tr><td><strong>Net</strong></td><td className={'num ' + (pnl.net >= 0 ? 'pos' : 'neg')}><strong>{money(pnl.net)}</strong></td></tr>
          </tbody>
        </table>
      </div>
      <div className="card">
        <div className="label" style={{ marginBottom: 8 }}>Balance Sheet {bs.balanced ? '' : '(unbalanced!)'}</div>
        <table>
          <tbody>
            <tr><td>Assets</td><td className="num">{money(bs.assets)}</td></tr>
            <tr><td>Liabilities</td><td className="num">{money(bs.liabilities)}</td></tr>
            <tr><td>Equity</td><td className="num">{money(bs.equity)}</td></tr>
            <tr><td>Retained earnings (net income)</td><td className="num">{money(bs.net_income)}</td></tr>
          </tbody>
        </table>
      </div>
    </>
  )
}
