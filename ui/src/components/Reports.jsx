import { useEffect, useState } from 'react'
import { invoke, money } from '../api.js'

export default function Reports() {
  const [pnl, setPnl] = useState(null)
  const [bs, setBs] = useState(null)
  const [err, setErr] = useState(null)

  useEffect(() => {
    Promise.all([invoke('report.pnl'), invoke('report.balance_sheet')])
      .then(([p, b]) => { setPnl(p); setBs(b) })
      .catch((e) => setErr(String(e)))
  }, [])

  if (err) return <p className="neg">{err}</p>
  if (!pnl || !bs) return <p>Loading…</p>

  return (
    <>
      <h1>Reports</h1>
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
