import { useEffect, useState } from 'react'
import { invoke } from '../api.js'

export default function Settings() {
  const [data, setData] = useState(null)
  const [drafts, setDrafts] = useState({})   // per-provider {api_key, model, base_url}
  const [msg, setMsg] = useState(null)
  const [err, setErr] = useState(null)

  function load() {
    invoke('settings.list_providers')
      .then((d) => {
        setData(d)
        const init = {}
        d.providers.forEach((p) => { init[p.provider] = { api_key: '', model: p.model, base_url: p.base_url } })
        setDrafts(init)
      })
      .catch((e) => setErr(String(e)))
  }
  useEffect(load, [])

  async function save(provider, makeActive) {
    setErr(null); setMsg(null)
    const d = drafts[provider] || {}
    const args = { provider, model: d.model, base_url: d.base_url, active: !!makeActive }
    if (d.api_key) args.api_key = d.api_key      // only send if the user typed one
    try {
      await invoke('settings.set_provider', args)
      setMsg(`Saved ${provider}${makeActive ? ' (now active)' : ''}.`)
      load()
    } catch (e) { setErr(String(e)) }
  }
  async function clearKey(provider) {
    try { await invoke('settings.clear_key', { provider }); setMsg(`Cleared ${provider} key.`); load() }
    catch (e) { setErr(String(e)) }
  }
  function setField(provider, field, value) {
    setDrafts((d) => ({ ...d, [provider]: { ...d[provider], [field]: value } }))
  }

  if (err && !data) return <p className="neg">{err}</p>
  if (!data) return <p>Loading…</p>

  return (
    <>
      <h1>Settings — AI providers</h1>
      <p className="muted" style={{ maxWidth: 600 }}>
        Keys are stored in the macOS Keychain (never in your book file). The active provider powers the Assistant.
      </p>
      {data.providers.map((p) => (
        <div className="card" key={p.provider} style={{ maxWidth: 600, marginBottom: 16 }}>
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: 12 }}>
            <strong style={{ textTransform: 'capitalize' }}>{p.provider}</strong>
            {p.active
              ? <span className="tag" style={{ color: 'var(--green)', borderColor: 'var(--green)' }}>active</span>
              : <button className="toggle" style={{ flex: 'none', padding: '4px 10px' }} onClick={() => save(p.provider, true)}>Make active</button>}
          </div>
          <label>API key {p.has_key && <span className="pos">· saved ✓</span>}
            <input type="password" placeholder={p.has_key ? '•••••••• (leave blank to keep)' : 'paste API key'}
              value={drafts[p.provider]?.api_key || ''} onChange={(e) => setField(p.provider, 'api_key', e.target.value)} />
          </label>
          <label>Model
            <input type="text" value={drafts[p.provider]?.model || ''} onChange={(e) => setField(p.provider, 'model', e.target.value)} />
          </label>
          <label>Base URL
            <input type="text" value={drafts[p.provider]?.base_url || ''} onChange={(e) => setField(p.provider, 'base_url', e.target.value)} />
          </label>
          <div style={{ display: 'flex', gap: 8 }}>
            <button className="primary" onClick={() => save(p.provider, p.active)}>Save</button>
            {p.has_key && <button className="toggle" style={{ flex: 'none' }} onClick={() => clearKey(p.provider)}>Clear key</button>}
          </div>
        </div>
      ))}
      {msg && <p className="pos">{msg}</p>}
      {err && <p className="neg">{err}</p>}
    </>
  )
}
