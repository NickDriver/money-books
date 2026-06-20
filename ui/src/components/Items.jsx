import { useEffect, useState } from 'react'
import { invoke, money } from '../api.js'

const toCents = (v) => Math.round(parseFloat(v || '0') * 100)

// Reusable service/expense line templates (D14): name + default price + linked category.
// Used to autofill invoice/bill lines.
export default function Items() {
  const [rows, setRows] = useState(null)
  const [accounts, setAccounts] = useState([])
  const [err, setErr] = useState(null)
  const [creating, setCreating] = useState(false)
  const [filter, setFilter] = useState('all') // 'all' | 'SERVICE' | 'EXPENSE'

  function reload() {
    invoke('item.list', {}).then((r) => setRows(r.items || [])).catch((e) => setErr(String(e)))
  }
  useEffect(() => {
    reload()
    invoke('account.list', { active_only: true }).then((r) => setAccounts(r.accounts)).catch((e) => setErr(String(e)))
  }, [])

  async function setActive(it, active, ev) {
    ev.stopPropagation()
    try { await invoke('item.set_active', { id: it.id, active }); reload() }
    catch (e) { setErr(String(e)) }
  }

  if (err) return <p className="neg">{err}</p>
  if (!rows) return <p>Loading…</p>

  const acctName = (id) => accounts.find((a) => a.id === id)?.name || ''
  const shown = rows.filter((it) => filter === 'all' || it.kind === filter)

  return (
    <>
      <h1>Items</h1>
      <p className="muted" style={{ marginTop: 0 }}>Reusable line templates for invoices &amp; bills — a name, default price, and category.</p>
      <div style={{ display: 'flex', gap: 8, marginBottom: 18, maxWidth: 360 }}>
        {['all', 'SERVICE', 'EXPENSE'].map((f) => (
          <button key={f} className={'toggle ' + (filter === f ? 'on' : '')} onClick={() => setFilter(f)}>
            {f === 'all' ? 'All' : f === 'SERVICE' ? 'Service' : 'Expense'}
          </button>
        ))}
      </div>
      {!creating && (
        <button className="primary" style={{ marginBottom: 16 }} onClick={() => setCreating(true)}>+ New item</button>
      )}
      {creating && (
        <ItemForm accounts={accounts}
          onDone={() => { setCreating(false); reload() }}
          onCancel={() => setCreating(false)} />
      )}

      <table>
        <thead>
          <tr><th>Name</th><th>Kind</th><th>Category</th><th className="num">Default price</th><th></th></tr>
        </thead>
        <tbody>
          {shown.length === 0 && <tr><td colSpan={5} className="muted">{rows.length === 0 ? 'No items yet.' : 'No items in this filter.'}</td></tr>}
          {shown.map((it) => (
            <tr key={it.id} style={it.is_active ? undefined : { opacity: 0.5 }}>
              <td>{it.name}{it.unit_label ? <span className="muted"> /{it.unit_label}</span> : null}{!it.is_active && <span className="tag" style={{ marginLeft: 8 }}>archived</span>}</td>
              <td><span className="tag">{it.kind === 'EXPENSE' ? 'Expense' : 'Service'}</span></td>
              <td>{acctName(it.default_account_id) || <span className="muted">—</span>}</td>
              <td className="num">{money(it.default_unit_price)}</td>
              <td className="num">
                {it.is_active
                  ? <button className="linkbtn" onClick={(e) => setActive(it, false, e)}>Archive</button>
                  : <button className="linkbtn" onClick={(e) => setActive(it, true, e)}>Restore</button>}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </>
  )
}

function ItemForm({ accounts, onDone, onCancel }) {
  const [kind, setKind] = useState('SERVICE')
  const [name, setName] = useState('')
  const [price, setPrice] = useState('')
  const [acct, setAcct] = useState('')
  const [unit, setUnit] = useState('')
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState(null)

  // services link to income categories, expense items to expense categories
  const categories = accounts.filter((a) => a.type === (kind === 'EXPENSE' ? 'EXPENSE' : 'INCOME'))

  async function submit() {
    const nm = name.trim()
    if (!nm) { setErr('Name required'); return }
    setBusy(true); setErr(null)
    try {
      await invoke('item.create', {
        kind, name: nm,
        default_unit_price: toCents(price),
        default_account_id: acct || null,
        unit_label: unit.trim() || null,
      })
      onDone()
    } catch (e) { setErr(String(e)); setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 520, marginBottom: 18 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
        <strong>New item</strong>
        <button className="btn-outline" type="button" onClick={onCancel}>Cancel</button>
      </div>
      <div style={{ display: 'flex', gap: 8, marginBottom: 12 }}>
        <button className={'toggle ' + (kind === 'SERVICE' ? 'on' : '')} type="button" onClick={() => { setKind('SERVICE'); setAcct('') }}>Service (sell)</button>
        <button className={'toggle ' + (kind === 'EXPENSE' ? 'on' : '')} type="button" onClick={() => { setKind('EXPENSE'); setAcct('') }}>Expense (buy)</button>
      </div>
      <label>Name<input value={name} onChange={(e) => setName(e.target.value)} placeholder="e.g. Hourly consulting" /></label>
      <div style={{ display: 'flex', gap: 12 }}>
        <label style={{ flex: 1 }}>Default price<input type="number" step="0.01" min="0" value={price} onChange={(e) => setPrice(e.target.value)} placeholder="0.00" /></label>
        <label style={{ flex: 1 }}>Unit label<input value={unit} onChange={(e) => setUnit(e.target.value)} placeholder="optional, e.g. hour" /></label>
      </div>
      <label>{kind === 'EXPENSE' ? 'Expense category' : 'Income category'}
        <select value={acct} onChange={(e) => setAcct(e.target.value)}>
          <option value="">— none —</option>
          {categories.map((a) => <option key={a.id} value={a.id}>{a.code} · {a.name}</option>)}
        </select>
      </label>
      <button className="btn-save filled" type="button" disabled={busy} onClick={submit} style={{ marginTop: 4 }}>Create item</button>
      {err && <p className="neg" style={{ marginTop: 12 }}>{err}</p>}
    </div>
  )
}
