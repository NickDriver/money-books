import { useEffect, useState } from 'react'
import { invoke, money } from '../api.js'

const roleLabel = (role) => (role === 'CATEGORY' ? 'Category' : role === 'ACCOUNT' ? 'Account' : 'System')

// Friendly "what is this?" choices (D6) that map to the underlying type + role.
const KINDS = [
  { label: 'Income category', type: 'INCOME', role: 'CATEGORY' },
  { label: 'Expense category', type: 'EXPENSE', role: 'CATEGORY' },
  { label: 'Bank / cash account (asset)', type: 'ASSET', role: 'ACCOUNT' },
  { label: 'Liability', type: 'LIABILITY', role: 'ACCOUNT' },
  { label: 'Equity', type: 'EQUITY', role: 'ACCOUNT' },
]

export default function Accounts() {
  const [rows, setRows] = useState(null)
  const [err, setErr] = useState(null)
  const [selected, setSelected] = useState(null) // account → show its ledger
  const [editing, setEditing] = useState(null)   // {mode:'new'} | {mode:'edit', acct}

  function reload() {
    invoke('account.list', {}).then((r) => setRows(r.accounts)).catch((e) => setErr(String(e)))
  }
  useEffect(reload, [])

  async function setActive(acct, active, ev) {
    ev.stopPropagation()
    try { await invoke('account.set_active', { id: acct.id, active }); reload() }
    catch (e) { setErr(String(e)) }
  }

  if (err) return <p className="neg">{err}</p>
  if (!rows) return <p>Loading…</p>
  if (selected) return <Ledger account={selected} onBack={() => setSelected(null)} />

  return (
    <>
      <h1>Accounts &amp; Categories</h1>
      {!editing && (
        <button className="primary" style={{ marginBottom: 16 }} onClick={() => setEditing({ mode: 'new' })}>+ New account / category</button>
      )}
      {editing && (
        <AccountForm editing={editing}
          onDone={() => { setEditing(null); reload() }}
          onCancel={() => setEditing(null)} />
      )}

      <table>
        <thead>
          <tr><th>Code</th><th>Name</th><th>Type</th><th>Kind</th><th></th></tr>
        </thead>
        <tbody>
          {rows.map((a) => {
            const system = a.role === 'SYSTEM'
            return (
              <tr key={a.id} className="rowlink" onClick={() => setSelected(a)} style={a.is_active ? undefined : { opacity: 0.5 }}>
                <td>{a.code || <span className="muted">—</span>}</td>
                <td>{a.name}{!a.is_active && <span className="tag" style={{ marginLeft: 8 }}>archived</span>}</td>
                <td>{a.type}</td>
                <td><span className="tag">{roleLabel(a.role)}</span></td>
                <td className="num" onClick={(e) => e.stopPropagation()}>
                  {system ? <span className="muted" style={{ fontSize: 12 }}>locked</span> : (
                    <>
                      <button className="linkbtn" onClick={(e) => { e.stopPropagation(); setEditing({ mode: 'edit', acct: a }) }}>Edit</button>
                      {a.is_active
                        ? <button className="linkbtn" onClick={(e) => setActive(a, false, e)}>Archive</button>
                        : <button className="linkbtn" onClick={(e) => setActive(a, true, e)}>Restore</button>}
                    </>
                  )}
                </td>
              </tr>
            )
          })}
        </tbody>
      </table>
      <p className="muted" style={{ fontSize: 12, marginTop: 10 }}>Click a row to view its ledger. Type can't change after creation (it would corrupt past reports).</p>
    </>
  )
}

function AccountForm({ editing, onDone, onCancel }) {
  const isEdit = editing.mode === 'edit'
  const acct = editing.acct
  const [name, setName] = useState(isEdit ? acct.name : '')
  const [code, setCode] = useState(isEdit ? (acct.code || '') : '')
  const [kindIdx, setKindIdx] = useState(0)
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState(null)

  async function submit() {
    const nm = name.trim()
    if (!nm) { setErr('Name required'); return }
    setBusy(true); setErr(null)
    try {
      if (isEdit) {
        await invoke('account.update', { id: acct.id, code: code.trim() || null, name: nm })
      } else {
        const k = KINDS[kindIdx]
        await invoke('account.create', { name: nm, code: code.trim() || null, type: k.type, role: k.role })
      }
      onDone()
    } catch (e) { setErr(String(e)); setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 520, marginBottom: 18 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
        <strong>{isEdit ? `Edit ${acct.name}` : 'New account / category'}</strong>
        <button className="btn-outline" type="button" onClick={onCancel}>Cancel</button>
      </div>
      <label>Name<input value={name} onChange={(e) => setName(e.target.value)} placeholder="e.g. Consulting Income" /></label>
      <label>Code<input value={code} onChange={(e) => setCode(e.target.value)} placeholder="optional, e.g. 4000" /></label>
      {isEdit ? (
        <p className="muted" style={{ fontSize: 13 }}>{acct.type} · {roleLabel(acct.role)} (type can't change)</p>
      ) : (
        <label>What is it?
          <select value={kindIdx} onChange={(e) => setKindIdx(Number(e.target.value))}>
            {KINDS.map((k, i) => <option key={k.label} value={i}>{k.label}</option>)}
          </select>
        </label>
      )}
      <button className="btn-save filled" type="button" disabled={busy} onClick={submit} style={{ marginTop: 4 }}>
        {isEdit ? 'Save' : 'Create'}
      </button>
      {err && <p className="neg" style={{ marginTop: 12 }}>{err}</p>}
    </div>
  )
}

function Ledger({ account, onBack }) {
  const [rows, setRows] = useState(null)
  const [err, setErr] = useState(null)

  useEffect(() => {
    invoke('report.ledger', { account_id: account.id })
      .then((r) => setRows(r.rows || []))
      .catch((e) => setErr(String(e)))
  }, [account.id])

  const creditNormal = account.type === 'INCOME' || account.type === 'LIABILITY' || account.type === 'EQUITY'
  const lastRunning = rows && rows.length ? rows[rows.length - 1].running : 0
  const naturalBalance = creditNormal ? -lastRunning : lastRunning

  return (
    <div style={{ maxWidth: 820 }}>
      <button className="linkbtn" onClick={onBack}>← Back to accounts</button>
      <h2 style={{ margin: '10px 0 2px' }}>{account.code} · {account.name}</h2>
      <div className="muted" style={{ marginBottom: 16 }}>
        {account.type} · {roleLabel(account.role)}
        {rows && rows.length > 0 && <> · balance <strong style={{ color: 'var(--text)' }}>{money(naturalBalance)}</strong></>}
      </div>

      {err && <p className="neg">{err}</p>}
      {!rows ? <p className="muted">Loading…</p> : (
        <>
          <table>
            <thead>
              <tr><th>Date</th><th>Description</th><th className="num">Debit</th><th className="num">Credit</th><th className="num">Balance</th></tr>
            </thead>
            <tbody>
              {rows.length === 0 && <tr><td colSpan={5} className="muted">No postings to this account yet.</td></tr>}
              {rows.map((r, i) => (
                <tr key={r.entry_id + ':' + i}>
                  <td>{r.date}</td>
                  <td>{r.memo || <span className="muted">—</span>}</td>
                  <td className="num">{r.amount > 0 ? money(r.amount) : ''}</td>
                  <td className="num">{r.amount < 0 ? money(-r.amount) : ''}</td>
                  <td className="num">{money(creditNormal ? -r.running : r.running)}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <p className="muted" style={{ fontSize: 12, marginTop: 10 }}>
            Running balance shown in this account's natural sign
            ({creditNormal ? 'credit-normal: credits increase it' : 'debit-normal: debits increase it'}).
          </p>
        </>
      )}
    </div>
  )
}
