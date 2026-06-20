// Read-only mode flag, shared via context. True when the window is a guest viewing a
// host's shared book (Phase 7b): the host's dispatch refuses every write regardless, so
// this only governs whether the UI *shows* write controls — hiding them keeps a guest from
// clicking buttons that would just error. Defaults to false (the normal owner app).
import { createContext, useContext } from 'react'

export const ReadOnlyContext = createContext(false)
export const useReadOnly = () => useContext(ReadOnlyContext)
