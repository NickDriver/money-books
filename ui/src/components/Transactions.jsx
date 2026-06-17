import { useEffect, useState } from 'react'
import { invoke, money } from '../api.js'

const STATUS_CLASS = { POSTED: 'st-open', REVERSED: 'st-void', REVERSAL: 'st-partial' }
// How each entry reads at a glance, by the account types it touches.
const FLOW = {
  INCOME:  { label: 'Income',   cls: 'pos' },
  EXPENSE: { label: 'Expense',  cls: 'neg' },
  OTHER:   { label: 'Transfer', cls: 'muted' },
}

// All recorded transactions (general ledger / journal) with Income and Expense filters.
// `initialFilter` ('all' | 'income' | 'expense') lets the dashboard cards deep-link in.
export default function Transactions({ initialFilter = 'all' }) {
  const [filter, setFilter] = useState(initialFilter)
  const [entries, setEntries] = useState([])
  const [total, setTotal] = useState(null)
  const [err, setErr] = useState(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => { setFilter(initialFilter) }, [initialFilter])

  useEffect(() => {
    setLoading(true); setErr(null); setTotal(null)
    const p = filter === 'all'
      ? invoke('report.journal', {}).then((r) => { setEntries(r.entries || []) })
      : invoke('report.category_txns', { type: filter === 'income' ? 'INCOME' : 'EXPENSE' })
          .then((r) => { setEntries(r.transactions || []); setTotal(r.total) })
    p.catch((e) => setErr(String(e))).finally(() => setLoading(false))
  }, [filter])

  return (
    <>
      <h1>Transactions</h1>
      <div style={{ display: 'flex', gap: 8, marginBottom: 18, maxWidth: 360 }}>
        {['all', 'income', 'expense'].map((f) => (
          <button key={f} className={'toggle ' + (filter === f ? 'on' : '')} onClick={() => setFilter(f)}>
            {f === 'all' ? 'All' : f === 'income' ? 'Income' : 'Expenses'}
          </button>
        ))}
      </div>

      {err && <p className="neg">{err}</p>}
      {loading ? <p className="muted">Loading…</p> : (
        filter === 'all' ? <JournalTable entries={entries} /> : <CategoryTable rows={entries} total={total} />
      )}
    </>
  )
}

function JournalTable({ entries }) {
  return (
    <table>
      <thead>
        <tr><th>Date</th><th>Description</th><th>Type</th><th>Status</th><th className="num">Amount</th></tr>
      </thead>
      <tbody>
        {entries.length === 0 && <tr><td colSpan={5} className="muted">No transactions yet.</td></tr>}
        {entries.map((e) => {
          const f = FLOW[e.flow] || FLOW.OTHER
          // accounting style: money-out (expense) in parentheses + red, income green, transfer neutral
          const shown = e.flow === 'EXPENSE' ? `(${money(e.amount)})` : money(e.amount)
          return (
            <tr key={e.entry_id}>
              <td>{e.date}</td>
              <td>{e.memo || <span className="muted">—</span>}</td>
              <td className={f.cls}>{f.label}</td>
              <td><span className={'tag ' + (STATUS_CLASS[e.status] || '')}>{e.status}</span></td>
              <td className={'num ' + f.cls}>{shown}</td>
            </tr>
          )
        })}
      </tbody>
    </table>
  )
}

function CategoryTable({ rows, total }) {
  return (
    <table>
      <thead>
        <tr><th>Date</th><th>Description</th><th>Category</th><th className="num">Amount</th></tr>
      </thead>
      <tbody>
        {rows.length === 0 && <tr><td colSpan={4} className="muted">Nothing here yet.</td></tr>}
        {rows.map((r, i) => (
          <tr key={r.entry_id + ':' + i}>
            <td>{r.date}</td>
            <td>{r.memo || <span className="muted">—</span>}</td>
            <td>{r.category_name}</td>
            <td className="num">{money(r.amount)}</td>
          </tr>
        ))}
        {rows.length > 0 && total != null && (
          <tr><td colSpan={3} style={{ fontWeight: 600 }}>Total</td><td className="num" style={{ fontWeight: 600 }}>{money(total)}</td></tr>
        )}
      </tbody>
    </table>
  )
}
