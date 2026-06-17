import { useEffect, useState } from 'react'
import { invoke } from '../api.js'

const today = () => new Date().toISOString().slice(0, 10)

export default function Record() {
  const [kind, setKind] = useState('income') // 'income' | 'expense'
  const [accounts, setAccounts] = useState([])
  const [date, setDate] = useState(today())
  const [amount, setAmount] = useState('')
  const [moneyAcct, setMoneyAcct] = useState('')
  const [category, setCategory] = useState('')
  const [memo, setMemo] = useState('')
  const [msg, setMsg] = useState(null)
  const [err, setErr] = useState(null)

  useEffect(() => {
    invoke('account.list', { active_only: true })
      .then((r) => setAccounts(r.accounts))
      .catch((e) => setErr(String(e)))
  }, [])

  const moneyAccounts = accounts.filter((a) => a.type === 'ASSET')
  const categories = accounts.filter((a) => a.type === (kind === 'income' ? 'INCOME' : 'EXPENSE'))

  async function submit(e) {
    e.preventDefault()
    setErr(null); setMsg(null)
    const cents = Math.round(parseFloat(amount) * 100)
    if (!cents || cents <= 0) { setErr('Enter a positive amount'); return }
    if (!moneyAcct || !category) { setErr('Pick an account and a category'); return }
    try {
      const method = kind === 'income' ? 'income.record' : 'expense.record'
      const args = kind === 'income'
        ? { date, amount: cents, deposit_account_id: moneyAcct, category_id: category, memo }
        : { date, amount: cents, pay_account_id: moneyAcct, category_id: category, memo }
      await invoke(method, args)
      setMsg(`Recorded ${kind} of $${(cents / 100).toFixed(2)}. The dashboard will reflect it.`)
      setAmount(''); setMemo('')
    } catch (e2) {
      setErr(String(e2))
    }
  }

  return (
    <>
      <h1>Record a transaction</h1>
      <div className="card" style={{ maxWidth: 520 }}>
        <div style={{ display: 'flex', gap: 8, marginBottom: 16 }}>
          <button className={'toggle ' + (kind === 'income' ? 'on' : '')} onClick={() => setKind('income')}>Income</button>
          <button className={'toggle ' + (kind === 'expense' ? 'on' : '')} onClick={() => setKind('expense')}>Expense</button>
        </div>
        <form onSubmit={submit}>
          <label>Date<input type="date" value={date} onChange={(e) => setDate(e.target.value)} /></label>
          <label>Amount<input type="number" step="0.01" min="0" placeholder="0.00" value={amount} onChange={(e) => setAmount(e.target.value)} /></label>
          <label>{kind === 'income' ? 'Deposit to' : 'Paid from'}
            <select value={moneyAcct} onChange={(e) => setMoneyAcct(e.target.value)}>
              <option value="">— select account —</option>
              {moneyAccounts.map((a) => <option key={a.id} value={a.id}>{a.code} · {a.name}</option>)}
            </select>
          </label>
          <label>Category
            <select value={category} onChange={(e) => setCategory(e.target.value)}>
              <option value="">— select category —</option>
              {categories.map((a) => <option key={a.id} value={a.id}>{a.code} · {a.name}</option>)}
            </select>
          </label>
          <label>Memo<input type="text" value={memo} onChange={(e) => setMemo(e.target.value)} placeholder="optional" /></label>
          <button type="submit" className="primary">Record {kind}</button>
        </form>
        {msg && <p className="pos" style={{ marginTop: 14 }}>{msg}</p>}
        {err && <p className="neg" style={{ marginTop: 14 }}>{err}</p>}
      </div>
    </>
  )
}
