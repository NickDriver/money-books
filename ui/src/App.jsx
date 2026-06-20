import { useEffect, useState } from 'react'
import { invoke, usingBridge } from './api.js'
import { ReadOnlyContext } from './readonly.js'
import Dashboard from './components/Dashboard.jsx'
import Record from './components/Record.jsx'
import Transactions from './components/Transactions.jsx'
import Invoices from './components/Invoices.jsx'
import Accounts from './components/Accounts.jsx'
import Items from './components/Items.jsx'
import Reports from './components/Reports.jsx'
import Wizard from './components/Wizard.jsx'
import Launcher from './components/Launcher.jsx'
import ShareModal from './components/Share.jsx'

const TABS = [
  { key: 'dashboard', label: 'Dashboard', el: Dashboard },
  { key: 'record', label: 'Record', el: Record },
  { key: 'transactions', label: 'Transactions', el: Transactions },
  { key: 'invoices', label: 'Invoices & Bills', el: Invoices },
  { key: 'accounts', label: 'Accounts & Categories', el: Accounts },
  { key: 'items', label: 'Items', el: Items },
  { key: 'reports', label: 'Reports', el: Reports },
]

// A guest (read-only) sees only the view tabs — the write-oriented Record/Accounts/Items
// tabs are hidden, and the remaining tabs hide their write controls (ReadOnlyContext).
const GUEST_TABS = new Set(['dashboard', 'transactions', 'invoices', 'reports'])

export default function App() {
  const [current, setCurrent] = useState(undefined) // undefined=checking, null=none, {path,name,read_only}=open
  const [showLauncher, setShowLauncher] = useState(false)
  const [showShare, setShowShare] = useState(false)
  const [onboarded, setOnboarded] = useState(null)
  const [nav, setNav] = useState({ tab: 'dashboard', params: {} })

  useEffect(() => {
    invoke('app.book_current')
      .then((r) => setCurrent(r.path || r.read_only ? { path: r.path, name: r.name, read_only: !!r.read_only } : null))
      .catch(() => setCurrent(null))
  }, [])

  const isGuest = !!current && !!current.read_only

  useEffect(() => {
    if (!current) return
    if (isGuest) { setOnboarded(true); return } // a guest never onboards — the host's book already exists
    invoke('book.status')
      .then((r) => setOnboarded(!!r.onboarded))
      .catch(() => setOnboarded(true))
  }, [current, isGuest])

  async function disconnect() {
    try { await invoke('app.share_disconnect') } catch { /* ignore */ }
    window.location.reload()
  }

  // Guest keepalive: poll a cheap read so that if the host stops sharing (or the link drops),
  // we notice within a couple seconds even when idle, and fall back to the launcher instead of
  // showing a frozen read-only view the host has already revoked.
  useEffect(() => {
    if (!isGuest) return
    let stop = false
    const t = setInterval(() => {
      invoke('book.status').catch(() => { if (!stop) { stop = true; clearInterval(t); window.location.reload() } })
    }, 2500)
    return () => { stop = true; clearInterval(t) }
  }, [isGuest])

  if (current === undefined) return null
  if (showLauncher || current === null) return <Launcher hasCurrent={!!current} onCancel={() => setShowLauncher(false)} />
  if (onboarded === null) return null
  if (!onboarded) return <Wizard onDone={() => setOnboarded(true)} />

  const tabs = isGuest ? TABS.filter((t) => GUEST_TABS.has(t.key)) : TABS
  const go = (tab, params = {}) => setNav({ tab, params })
  const activeTab = tabs.find((t) => t.key === nav.tab) || tabs[0]
  const Active = activeTab.el

  return (
    <ReadOnlyContext.Provider value={isGuest}>
      <div className="app">
        <aside className="sidebar">
          <div className="brand">
            Money Books
            {current.name && <div className="company" title={current.path || ''}>{current.name}</div>}
            {isGuest && <div className="tag st-open" style={{ marginTop: 6, display: 'inline-block' }}>read-only</div>}
          </div>
          <nav className="nav">
            {tabs.map((t) => (
              <button key={t.key} className={t.key === activeTab.key ? 'active' : ''} onClick={() => go(t.key)}>
                {t.label}
              </button>
            ))}
          </nav>
          {isGuest
            ? <button className="nav-switch" onClick={disconnect}>⤬ Disconnect</button>
            : <>
                <button className="nav-switch" onClick={() => setShowShare(true)}>⇆ Share book</button>
                <button className="nav-switch" onClick={() => setShowLauncher(true)}>⇄ Switch company</button>
              </>}
        </aside>
        <main className="main">
          {!usingBridge && <div className="banner">Preview mode — showing mock data (native engine not attached)</div>}
          {isGuest && <div className="banner">Shared book — read-only. You can view and export; changes are disabled.</div>}
          <Active go={go} initialFilter={nav.params.filter || 'all'} />
        </main>
        {showShare && <ShareModal onClose={() => setShowShare(false)} />}
      </div>
    </ReadOnlyContext.Provider>
  )
}
