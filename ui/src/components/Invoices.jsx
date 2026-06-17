import { useEffect, useState } from 'react'
import { invoke, money } from '../api.js'

const today = () => new Date().toISOString().slice(0, 10)
const toCents = (v) => Math.round(parseFloat(v || '0') * 100)
const toCenti = (v) => Math.round(parseFloat(v || '1') * 100) // qty x100
const lineAmount = (l) => Math.round((toCenti(l.qty) / 100) * toCents(l.price))

// Per-kind config so one component drives both the receivable (invoice) and payable (bill) sides.
const KIND = {
  invoice: {
    noun: 'invoice', Noun: 'Invoice', who: 'Customer', cpKind: 'CUSTOMER',
    acctType: 'INCOME', acctLabel: 'Income category', itemKind: 'SERVICE',
    listMethod: 'invoice.list', listKey: 'invoices',
    get: 'invoice.get', idArg: 'invoice_id',
    create: 'invoice.create', addLine: 'invoice.add_line', removeLine: 'invoice.remove_line',
    update: 'invoice.update', post: 'invoice.issue', revert: 'invoice.revert',
    postVerb: 'Issue', postedLabel: 'issued', payTarget: 'INVOICE',
  },
  bill: {
    noun: 'bill', Noun: 'Bill', who: 'Vendor', cpKind: 'VENDOR',
    acctType: 'EXPENSE', acctLabel: 'Expense category', itemKind: 'EXPENSE',
    listMethod: 'bill.list', listKey: 'bills',
    get: 'bill.get', idArg: 'bill_id',
    create: 'bill.create', addLine: 'bill.add_line', removeLine: 'bill.remove_line',
    update: 'bill.update', post: 'bill.enter', revert: 'bill.revert',
    postVerb: 'Enter', postedLabel: 'entered', payTarget: 'BILL',
  },
}

const STATUS_CLASS = { DRAFT: 'st-draft', OPEN: 'st-open', PARTIAL: 'st-partial', PAID: 'st-paid', VOID: 'st-void' }

export default function Invoices() {
  const [kind, setKind] = useState('invoice')
  const k = KIND[kind]

  const [rows, setRows] = useState([])
  const [accounts, setAccounts] = useState([])
  const [counterparties, setCounterparties] = useState([])
  const [items, setItems] = useState([])
  const [err, setErr] = useState(null)
  const [view, setView] = useState({ mode: 'list' }) // 'list' | 'new' | {mode:'detail', id}

  function reloadList() {
    invoke(k.listMethod, {}).then((r) => setRows(r[k.listKey] || [])).catch((e) => setErr(String(e)))
  }
  useEffect(() => {
    setErr(null); setView({ mode: 'list' })
    reloadList()
    invoke('account.list', { active_only: true }).then((r) => setAccounts(r.accounts)).catch((e) => setErr(String(e)))
    invoke('counterparty.list', { active_only: true }).then((r) => setCounterparties(r.counterparties || [])).catch((e) => setErr(String(e)))
    invoke('item.list', { active_only: true }).then((r) => setItems(r.items || [])).catch(() => {})
  }, [kind]) // eslint-disable-line react-hooks/exhaustive-deps

  function switchKind(nk) { setKind(nk) }

  return (
    <>
      <h1>Invoices &amp; Bills</h1>
      <div style={{ display: 'flex', gap: 8, marginBottom: 18, maxWidth: 360 }}>
        <button className={'toggle ' + (kind === 'invoice' ? 'on' : '')} onClick={() => switchKind('invoice')}>Invoices (owed to me)</button>
        <button className={'toggle ' + (kind === 'bill' ? 'on' : '')} onClick={() => switchKind('bill')}>Bills (I owe)</button>
      </div>

      {err && <p className="neg">{err}</p>}

      {view.mode === 'detail' ? (
        <Detail k={k} id={view.id} accounts={accounts} counterparties={counterparties} items={items}
          onCounterparties={setCounterparties}
          onBack={() => { setView({ mode: 'list' }); reloadList() }} />
      ) : view.mode === 'new' ? (
        <DocEditor k={k} mode="create" accounts={accounts} counterparties={counterparties} items={items}
          onCounterparties={setCounterparties}
          onDone={() => { setView({ mode: 'list' }); reloadList() }}
          onCancel={() => setView({ mode: 'list' })} />
      ) : (
        <>
          <button className="primary" style={{ marginBottom: 16 }} onClick={() => setView({ mode: 'new' })}>+ New {k.noun}</button>
          <table>
            <thead>
              <tr><th>Number</th><th>{k.who}</th><th>Issued</th><th>Due</th><th>Status</th><th className="num">Total</th></tr>
            </thead>
            <tbody>
              {rows.length === 0 && <tr><td colSpan={6} className="muted">No {k.noun}s yet.</td></tr>}
              {rows.map((r) => (
                <tr key={r.id} className="rowlink" onClick={() => setView({ mode: 'detail', id: r.id })}>
                  <td>{r.number || <span className="muted">—</span>}</td>
                  <td>{r.counterparty_name}</td>
                  <td>{r.issue_date || <span className="muted">draft</span>}</td>
                  <td>{r.due_date || <span className="muted">—</span>}</td>
                  <td><span className={'tag ' + (STATUS_CLASS[r.status] || '')}>{r.status}</span></td>
                  <td className="num">{money(r.total)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}
    </>
  )
}

function Detail({ k, id, accounts, counterparties, items, onCounterparties, onBack }) {
  const [doc, setDoc] = useState(null)
  const [editing, setEditing] = useState(false)
  const [paying, setPaying] = useState(false)
  const [applying, setApplying] = useState(false)
  const [credit, setCredit] = useState(0)
  const [err, setErr] = useState(null)
  const [busy, setBusy] = useState(false)

  function load() {
    invoke(k.get, { id }).then((d) => {
      setDoc(d)
      if (d.counterparty_id) {
        invoke('counterparty.balance', { counterparty_id: d.counterparty_id, target: k.payTarget })
          .then((b) => setCredit(b.credit_available || 0)).catch(() => setCredit(0))
      }
    }).catch((e) => setErr(String(e)))
  }
  useEffect(() => { load() }, [id]) // eslint-disable-line react-hooks/exhaustive-deps

  if (err) return <><button className="linkbtn" onClick={onBack}>← Back</button><p className="neg">{err}</p></>
  if (!doc) return <p className="muted">Loading…</p>

  const cashAccounts = accounts.filter((a) => a.type === 'ASSET')
  const isDraft = doc.status === 'DRAFT'
  const isOpen = doc.status === 'OPEN'
  const payable = doc.status === 'OPEN' || doc.status === 'PARTIAL'

  if (editing) {
    return <DocEditor k={k} mode="edit" doc={doc} accounts={accounts} counterparties={counterparties} items={items}
      onCounterparties={onCounterparties}
      onDone={() => { setEditing(false); load() }} onCancel={() => setEditing(false)} />
  }

  async function reopen() {
    setBusy(true); setErr(null)
    try { await invoke(k.revert, { id }); setEditing(true) }
    catch (e) { setErr(String(e)); setBusy(false) }
  }

  return (
    <div style={{ maxWidth: 760 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 14 }}>
        <button className="linkbtn" onClick={onBack}>← Back</button>
        <span className={'tag ' + (STATUS_CLASS[doc.status] || '')}>{doc.status}</span>
      </div>

      <h2 style={{ margin: '0 0 4px' }}>{k.Noun} {doc.number || ''}</h2>
      <div className="muted" style={{ marginBottom: 16 }}>
        {doc.counterparty_name}
        {doc.issue_date ? ` · ${k.postedLabel} ${doc.issue_date}` : ' · draft'}
        {doc.due_date ? ` · due ${doc.due_date}` : ''}
      </div>
      {doc.memo && <p style={{ marginTop: 0 }}>{doc.memo}</p>}

      <table>
        <thead><tr><th>Description</th><th className="num">Qty</th><th className="num">Unit</th><th>Category</th><th className="num">Amount</th></tr></thead>
        <tbody>
          {(doc.lines || []).map((l) => (
            <tr key={l.id}>
              <td>{l.description}{l.is_tax ? <span className="tag" style={{ marginLeft: 6 }}>tax</span> : null}</td>
              <td className="num">{(l.qty_centi / 100)}</td>
              <td className="num">{money(l.unit_price)}</td>
              <td>{l.account_name}</td>
              <td className="num">{money(l.line_total)}</td>
            </tr>
          ))}
          <tr><td colSpan={4} style={{ fontWeight: 600 }}>Total</td><td className="num" style={{ fontWeight: 600 }}>{money(doc.total)}</td></tr>
        </tbody>
      </table>

      {credit > 0 && (
        <p className="muted" style={{ fontSize: 13, marginTop: 4 }}>
          {k.who} has <strong className="pos">{money(credit)}</strong> credit available
          {payable ? ' — you can apply it to this ' + k.noun + '.' : '.'}
        </p>
      )}

      <div style={{ display: 'flex', gap: 8, marginTop: 14, flexWrap: 'wrap' }}>
        {isDraft && <button className="btn-save filled" onClick={() => setEditing(true)}>Edit</button>}
        {isOpen && <button className="btn-outline" disabled={busy} onClick={reopen}>Reopen to edit</button>}
        {payable && <button className="btn-save filled" onClick={() => setPaying(true)}>Record payment</button>}
        {payable && credit > 0 && <button className="btn-outline" onClick={() => setApplying(true)}>Apply credit</button>}
      </div>
      {isOpen && <p className="muted" style={{ fontSize: 12, marginTop: 8 }}>
        Reopening posts a reversing entry and returns this {k.noun} to draft so you can edit it, then re-{k.postVerb.toLowerCase()} it.</p>}
      {!isDraft && !isOpen && !payable && <p className="muted" style={{ fontSize: 12, marginTop: 8 }}>
        This {k.noun} is {doc.status.toLowerCase()} and locked. Corrections require unapplying its payment.</p>}
      {err && <p className="neg">{err}</p>}

      {paying && (
        <PaymentForm k={k} row={{ id: doc.id, number: doc.number, counterparty_name: doc.counterparty_name, total: doc.total }}
          cashAccounts={cashAccounts}
          onDone={() => { setPaying(false); load() }} onCancel={() => setPaying(false)} />
      )}
      {applying && (
        <ApplyCreditForm k={k} doc={doc} credit={credit}
          onDone={() => { setApplying(false); load() }} onCancel={() => setApplying(false)} />
      )}
    </div>
  )
}

function ApplyCreditForm({ k, doc, credit, onDone, onCancel }) {
  const remaining = doc.total - (doc.paid || 0)
  const max = Math.min(credit, remaining > 0 ? remaining : credit)
  const [amount, setAmount] = useState((max / 100).toFixed(2))
  const [date, setDate] = useState(new Date().toISOString().slice(0, 10))
  const [err, setErr] = useState(null)
  const [busy, setBusy] = useState(false)

  async function submit() {
    setBusy(true); setErr(null)
    const cents = Math.round(parseFloat(amount) * 100)
    if (!cents || cents <= 0) { setErr('Enter an amount'); setBusy(false); return }
    try {
      await invoke('credit.apply', { date, counterparty_id: doc.counterparty_id, target: k.payTarget, target_id: doc.id, amount: cents })
      onDone()
    } catch (e) { setErr(String(e)); setBusy(false) }
  }

  return (
    <div className="card" style={{ marginTop: 14, maxWidth: 420 }}>
      <strong>Apply credit — {doc.number || k.noun} ({doc.counterparty_name})</strong>
      <p className="muted" style={{ fontSize: 13, margin: '6px 0 10px' }}>
        {k.who} has {money(credit)} credit. This settles the {k.noun} without a cash payment.
      </p>
      <label>Date<input type="date" value={date} onChange={(e) => setDate(e.target.value)} /></label>
      <label>Amount<input value={amount} onChange={(e) => setAmount(e.target.value)} inputMode="decimal" /></label>
      {err && <p className="neg" style={{ fontSize: 13 }}>{err}</p>}
      <div style={{ display: 'flex', gap: 8, marginTop: 4 }}>
        <button className="btn-save filled" type="button" disabled={busy} onClick={submit}>Apply credit</button>
        <button className="btn-save outline" type="button" onClick={onCancel}>Cancel</button>
      </div>
    </div>
  )
}

function DocEditor({ k, mode, doc, accounts, counterparties, items, onCounterparties, onDone, onCancel }) {
  const categories = accounts.filter((a) => a.type === k.acctType)
  const firstCat = categories[0]?.id || ''
  const lineItems = (items || []).filter((it) => it.kind === k.itemKind)
  const blankLine = () => ({ description: '', qty: '1', price: '', account_id: firstCat })
  const fromDocLines = () => (doc?.lines?.length
    ? doc.lines.map((l) => ({ description: l.description, qty: String(l.qty_centi / 100),
        price: (l.unit_price / 100).toFixed(2), account_id: l.account_id }))
    : [blankLine()])

  const [cp, setCp] = useState(doc?.counterparty_id || '')
  const [number, setNumber] = useState(doc?.number || '')
  const [issueDate, setIssueDate] = useState(doc?.issue_date || today())
  const [dueDate, setDueDate] = useState(doc?.due_date || '')
  const [memo, setMemo] = useState(doc?.memo || '')
  const [lines, setLines] = useState(fromDocLines)
  const [newCp, setNewCp] = useState('')
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState(null)

  useEffect(() => {
    if (!firstCat) return
    setLines((ls) => ls.map((l) => (l.account_id ? l : { ...l, account_id: firstCat })))
  }, [firstCat])

  function setLine(i, patch) { setLines((ls) => ls.map((l, j) => (j === i ? { ...l, ...patch } : l))) }
  const addLine = () => setLines((ls) => [...ls, blankLine()])
  const removeLine = (i) => setLines((ls) => (ls.length > 1 ? ls.filter((_, j) => j !== i) : ls))
  function addFromItem(itemId) {
    const it = lineItems.find((x) => x.id === itemId)
    if (!it) return
    const line = {
      description: it.name,
      qty: '1',
      price: (it.default_unit_price / 100).toFixed(2),
      account_id: it.default_account_id || firstCat,
    }
    // fill the first blank line, otherwise append
    setLines((ls) => {
      const idx = ls.findIndex((l) => !l.description.trim() && !l.price)
      if (idx >= 0) return ls.map((l, j) => (j === idx ? line : l))
      return [...ls, line]
    })
  }
  const total = lines.reduce((s, l) => s + lineAmount(l), 0)

  async function addCounterparty() {
    const name = newCp.trim()
    if (!name) return
    try {
      const r = await invoke('counterparty.create', { name, kind: k.cpKind })
      onCounterparties([...counterparties, { id: r.id, name, kind: k.cpKind }])
      setCp(r.id); setNewCp('')
    } catch (e) { setErr(String(e)) }
  }

  async function submit(issue) {
    setErr(null)
    if (!cp) { setErr(`Pick a ${k.who.toLowerCase()}`); return }
    const valid = lines.filter((l) => l.description.trim() && toCents(l.price) > 0 && l.account_id)
    if (valid.length === 0) { setErr('Add at least one line with a description, price, and category'); return }
    setBusy(true)
    try {
      let docId
      if (mode === 'create') {
        const created = await invoke(k.create, { counterparty_id: cp, number: number || null, due_date: dueDate || null, memo: memo || null })
        docId = created.id
      } else {
        docId = doc.id
        await invoke(k.update, { id: docId, number: number || null, due_date: dueDate || null, memo: memo || null })
        // no line-level update in the engine; resync = drop existing lines, re-add the current set
        for (const l of (doc.lines || [])) await invoke(k.removeLine, { line_id: l.id })
      }
      for (const l of valid) {
        await invoke(k.addLine, {
          [k.idArg]: docId,
          description: l.description.trim(),
          qty_centi: toCenti(l.qty),
          unit_price: toCents(l.price),
          account_id: l.account_id,
        })
      }
      if (issue) await invoke(k.post, { [k.idArg]: docId, issue_date: issueDate })
      onDone()
    } catch (e) { setErr(String(e)); setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 760, marginBottom: 20 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
        <strong>{mode === 'create' ? `New ${k.noun}` : `Edit ${k.noun}`}</strong>
        <button className="btn-outline" type="button" onClick={onCancel}>Cancel</button>
      </div>

      <label>{k.who}
        <select value={cp} onChange={(e) => setCp(e.target.value)}>
          <option value="">— select —</option>
          {counterparties.map((c) => <option key={c.id} value={c.id}>{c.name}{c.kind === 'BOTH' ? '' : ` (${c.kind.toLowerCase()})`}</option>)}
        </select>
      </label>
      <div style={{ display: 'flex', gap: 8, marginBottom: 12, alignItems: 'center' }}>
        <input style={{ flex: 2, minWidth: 0, marginTop: 0 }} placeholder={`New ${k.who.toLowerCase()} name`} value={newCp} onChange={(e) => setNewCp(e.target.value)} />
        <button className="btn-outline" style={{ flex: 1 }} type="button" onClick={addCounterparty}>+ Add</button>
      </div>

      <div style={{ display: 'flex', gap: 12 }}>
        <label style={{ flex: 1 }}>Number<input value={number} onChange={(e) => setNumber(e.target.value)} placeholder="optional" /></label>
        <label style={{ flex: 1 }}>{k.postVerb} date<input type="date" value={issueDate} onChange={(e) => setIssueDate(e.target.value)} /></label>
        <label style={{ flex: 1 }}>Due date<input type="date" value={dueDate} onChange={(e) => setDueDate(e.target.value)} /></label>
      </div>
      <label>Memo<input value={memo} onChange={(e) => setMemo(e.target.value)} placeholder="optional" /></label>

      <div style={{ marginTop: 8, marginBottom: 6, display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
        <span style={{ color: 'var(--muted)', fontSize: 13 }}>Line items</span>
        {lineItems.length > 0 && (
          <select value="" onChange={(e) => { addFromItem(e.target.value); e.target.value = '' }} style={{ width: 'auto', marginTop: 0, maxWidth: 220 }}>
            <option value="">+ Add from item…</option>
            {lineItems.map((it) => <option key={it.id} value={it.id}>{it.name} ({money(it.default_unit_price)})</option>)}
          </select>
        )}
      </div>
      {lines.map((l, i) => (
        <div key={i} className="linerow">
          <input style={{ flex: 3 }} placeholder="Description" value={l.description} onChange={(e) => setLine(i, { description: e.target.value })} />
          <input style={{ flex: 1 }} type="number" step="0.01" min="0" placeholder="Qty" value={l.qty} onChange={(e) => setLine(i, { qty: e.target.value })} />
          <input style={{ flex: 1 }} type="number" step="0.01" min="0" placeholder="Price" value={l.price} onChange={(e) => setLine(i, { price: e.target.value })} />
          <select style={{ flex: 2 }} value={l.account_id} onChange={(e) => setLine(i, { account_id: e.target.value })}>
            <option value="">{k.acctLabel}</option>
            {categories.map((a) => <option key={a.id} value={a.id}>{a.code} · {a.name}</option>)}
          </select>
          <button className="btn-outline btn-icon" type="button" title="Remove line" onClick={() => removeLine(i)}>✕</button>
        </div>
      ))}
      <button className="btn-outline" type="button" style={{ marginTop: 4 }} onClick={addLine}>+ Add line</button>

      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginTop: 16 }}>
        <div className="muted">Total <strong style={{ color: 'var(--text)' }}>{money(total)}</strong></div>
        <div style={{ display: 'flex', gap: 8 }}>
          <button className="btn-save outline" type="button" disabled={busy} onClick={() => submit(false)}>Save as draft</button>
          <button className="btn-save filled" type="button" disabled={busy} onClick={() => submit(true)}>Save &amp; {k.postVerb.toLowerCase()}</button>
        </div>
      </div>
      {err && <p className="neg" style={{ marginTop: 12 }}>{err}</p>}
    </div>
  )
}

function PaymentForm({ k, row, cashAccounts, onDone, onCancel }) {
  const [date, setDate] = useState(today())
  const [amount, setAmount] = useState((row.total / 100).toFixed(2))
  const [cash, setCash] = useState('')
  const [busy, setBusy] = useState(false)
  const [err, setErr] = useState(null)

  async function submit() {
    setErr(null)
    const cents = toCents(amount)
    if (!cents || cents <= 0) { setErr('Enter a positive amount'); return }
    if (!cash) { setErr('Pick a cash account'); return }
    setBusy(true)
    try {
      await invoke('payment.record', { date, amount: cents, cash_account_id: cash, target: k.payTarget, target_id: row.id })
      onDone()
    } catch (e) { setErr(String(e)); setBusy(false) }
  }

  return (
    <div className="card" style={{ maxWidth: 520, marginTop: 16 }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
        <strong>Record payment — {row.number || k.noun} ({row.counterparty_name})</strong>
        <button className="btn-outline" type="button" onClick={onCancel}>Cancel</button>
      </div>
      <label>Date<input type="date" value={date} onChange={(e) => setDate(e.target.value)} /></label>
      <label>Amount<input type="number" step="0.01" min="0" value={amount} onChange={(e) => setAmount(e.target.value)} /></label>
      <label>{k.noun === 'invoice' ? 'Deposit to' : 'Paid from'}
        <select value={cash} onChange={(e) => setCash(e.target.value)}>
          <option value="">— select cash account —</option>
          {cashAccounts.map((a) => <option key={a.id} value={a.id}>{a.code} · {a.name}</option>)}
        </select>
      </label>
      <button className="btn-save filled" type="button" disabled={busy} onClick={submit} style={{ marginTop: 4 }}>Record payment</button>
      {err && <p className="neg" style={{ marginTop: 12 }}>{err}</p>}
    </div>
  )
}
