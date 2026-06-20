import { useEffect, useState } from 'react'
import { invoke, usingBridge } from './api.js'
import Dashboard from './components/Dashboard.jsx'
import Record from './components/Record.jsx'
import Transactions from './components/Transactions.jsx'
import Invoices from './components/Invoices.jsx'
import Accounts from './components/Accounts.jsx'
import Items from './components/Items.jsx'
import Reports from './components/Reports.jsx'
import Wizard from './components/Wizard.jsx'
import Launcher from './components/Launcher.jsx'

const TABS = [
  { key: 'dashboard', label: 'Dashboard', el: Dashboard },
  { key: 'record', label: 'Record', el: Record },
  { key: 'transactions', label: 'Transactions', el: Transactions },
  { key: 'invoices', label: 'Invoices & Bills', el: Invoices },
  { key: 'accounts', label: 'Accounts & Categories', el: Accounts },
  { key: 'items', label: 'Items', el: Items },
  { key: 'reports', label: 'Reports', el: Reports },
]

export default function App() {
  const [current, setCurrent] = useState(undefined) // undefined=checking, null=none, {path,name}=open
  const [showLauncher, setShowLauncher] = useState(false)
  const [onboarded, setOnboarded] = useState(null)
  const [nav, setNav] = useState({ tab: 'dashboard', params: {} })

  useEffect(() => {
    invoke('app.book_current')
      .then((r) => setCurrent(r.path ? { path: r.path, name: r.name } : null))
      .catch(() => setCurrent(null))
  }, [])

  useEffect(() => {
    if (!current) return
    invoke('book.status')
      .then((r) => setOnboarded(!!r.onboarded))
      .catch(() => setOnboarded(true))
  }, [current])

  if (current === undefined) return null
  if (showLauncher || current === null) return <Launcher hasCurrent={!!current} onCancel={() => setShowLauncher(false)} />
  if (onboarded === null) return null
  if (!onboarded) return <Wizard onDone={() => setOnboarded(true)} />

  const go = (tab, params = {}) => setNav({ tab, params })
  const Active = TABS.find((t) => t.key === nav.tab).el

  return (
    <div className="app">
      <aside className="sidebar">
        <div className="brand">
          Money Books
          {current.name && <div className="company" title={current.path}>{current.name}</div>}
        </div>
        <nav className="nav">
          {TABS.map((t) => (
            <button key={t.key} className={t.key === nav.tab ? 'active' : ''} onClick={() => go(t.key)}>
              {t.label}
            </button>
          ))}
        </nav>
        <button className="nav-switch" onClick={() => setShowLauncher(true)}>⇄ Switch company</button>
      </aside>
      <main className="main">
        {!usingBridge && <div className="banner">Preview mode — showing mock data (native engine not attached)</div>}
        <Active go={go} initialFilter={nav.params.filter || 'all'} />
      </main>
    </div>
  )
}
