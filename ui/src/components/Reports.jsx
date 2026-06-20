import { useEffect, useState } from 'react'
import { invoke, money, downloadCsv } from '../api.js'

// Card title on the left, its CSV export action in the right corner.
function CardHead({ title, method, busy, saved, onExport }) {
  const label = busy === method ? 'Exporting…'
    : saved?.method === method ? `Saved ${saved.file}`
    : 'Export CSV'
  return (
    <div className="card-head">
      <div className="label">{title}</div>
      <button className="linkbtn" onClick={() => onExport(method)} disabled={busy === method}>{label}</button>
    </div>
  )
}

export default function Reports() {
  const [pnl, setPnl] = useState(null)
  const [bs, setBs] = useState(null)
  const [err, setErr] = useState(null)
  const [busy, setBusy] = useState(null)
  const [saved, setSaved] = useState(null)   // { method, file }

  useEffect(() => {
    Promise.all([invoke('report.pnl'), invoke('report.balance_sheet')])
      .then(([p, b]) => { setPnl(p); setBs(b) })
      .catch((e) => setErr(String(e)))
  }, [])

  async function exportCsv(method) {
    setBusy(method); setSaved(null); setErr(null)
    try {
      const file = await downloadCsv(method)
      if (file) setSaved({ method, file })   // null = user cancelled the native Save dialog
    } catch (e) {
      setErr(String(e))
    } finally {
      setBusy(null)
    }
  }

  const head = (title, method) => (
    <CardHead title={title} method={method} busy={busy} saved={saved} onExport={exportCsv} />
  )

  if (err && !pnl) return <p className="neg">{err}</p>
  if (!pnl || !bs) return <p>Loading…</p>

  return (
    <>
      <h1>Reports</h1>
      {err && <p className="neg" style={{ marginBottom: 12 }}>{err}</p>}

      <div className="card" style={{ marginBottom: 16 }}>
        {head('Profit & Loss', 'export.pnl')}
        <table>
          <tbody>
            <tr><td>Income</td><td className="num">{money(pnl.income)}</td></tr>
            <tr><td>Expenses</td><td className="num">{money(pnl.expense)}</td></tr>
            <tr><td><strong>Net</strong></td><td className={'num ' + (pnl.net >= 0 ? 'pos' : 'neg')}><strong>{money(pnl.net)}</strong></td></tr>
          </tbody>
        </table>
      </div>

      <div className="card" style={{ marginBottom: 16 }}>
        {head(`Balance Sheet${bs.balanced ? '' : ' (unbalanced!)'}`, 'export.balance_sheet')}
        <table>
          <tbody>
            <tr><td>Assets</td><td className="num">{money(bs.assets)}</td></tr>
            <tr><td>Liabilities</td><td className="num">{money(bs.liabilities)}</td></tr>
            <tr><td>Equity</td><td className="num">{money(bs.equity)}</td></tr>
            <tr><td>Retained earnings (net income)</td><td className="num">{money(bs.net_income)}</td></tr>
          </tbody>
        </table>
      </div>

      <div className="card" style={{ marginBottom: 16 }}>
        {head('Trial Balance', 'export.trial_balance')}
        <p className="label" style={{ margin: 0 }}>Every account’s debit/credit balance — the accountant’s starting point.</p>
      </div>

      <div className="card" style={{ marginBottom: 16 }}>
        {head('General Ledger', 'export.general_ledger')}
        <p className="label" style={{ margin: 0 }}>Every posting by account, with a running balance.</p>
      </div>

      <div className="card">
        {head('Journal', 'export.journal')}
        <p className="label" style={{ margin: 0 }}>Full transaction listing, including reversals (audit view).</p>
      </div>
    </>
  )
}
