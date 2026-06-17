import { useState } from 'react'
import { invoke } from '../api.js'

export default function Chat() {
  const [msgs, setMsgs] = useState([])
  const [input, setInput] = useState('')
  const [busy, setBusy] = useState(false)

  async function send(e) {
    e.preventDefault()
    const text = input.trim()
    if (!text || busy) return
    setMsgs((m) => [...m, { role: 'user', text }])
    setInput('')
    setBusy(true)
    try {
      const r = await invoke('agent.send', { message: text })
      setMsgs((m) => [...m, { role: 'assistant', text: r.reply }])
    } catch (err) {
      setMsgs((m) => [...m, { role: 'error', text: String(err) }])
    } finally {
      setBusy(false)
    }
  }

  return (
    <>
      <h1>Assistant</h1>
      <div className="chat">
        <div className="chat-log">
          {msgs.length === 0 && (
            <p className="muted">Ask about your books, or tell me to record something —
              e.g. “record $2,500 consulting income to checking” or “what’s my P&amp;L this year?”</p>
          )}
          {msgs.map((m, i) => (
            <div key={i} className={'bubble ' + m.role}>{m.text}</div>
          ))}
          {busy && (
            <div className="bubble assistant typing-bubble" aria-label="Assistant is thinking">
              <span className="typing"><i /><i /><i /></span>
            </div>
          )}
        </div>
        <form className="chat-input" onSubmit={send}>
          <input value={input} onChange={(e) => setInput(e.target.value)} placeholder="Message the assistant…" />
          <button type="submit" className="primary" disabled={busy}>Send</button>
        </form>
      </div>
    </>
  )
}
