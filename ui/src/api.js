// Bridge to the C engine. The native shell binds `window.mbInvoke(method, argsJson)`
// (via webview_bind) which returns a Promise resolving to the JSON result string.
// In a plain browser (no native shell) we fall back to a small mock so the UI runs
// during front-end development.

const hasBridge = typeof window !== 'undefined' && typeof window.mbInvoke === 'function'

export async function invoke(method, args = {}) {
  if (hasBridge) {
    // webview already JSON-parses the value we pass to webview_return, so this
    // resolves to a plain object — do NOT JSON.parse it again.
    const res = await window.mbInvoke(method, JSON.stringify(args))
    if (res && res.error) throw new Error(`${res.error.code}: ${res.error.message}`)
    return res
  }
  return mock(method, args)
}

// money is integer cents on the wire; format for display
export function money(cents) {
  const neg = cents < 0
  const v = Math.abs(cents)
  return (neg ? '-' : '') + '$' + (Math.floor(v / 100)).toLocaleString() + '.' + String(v % 100).padStart(2, '0')
}

export const usingBridge = hasBridge

// ---- dev mock (browser only) ----
function mock(method, args) {
  switch (method) {
    case 'account.list':
      return {
        accounts: [
          { id: '1', code: '1000', name: 'Business Checking', type: 'ASSET', role: 'ACCOUNT', is_active: true },
          { id: '2', code: '4000', name: 'Consulting Income', type: 'INCOME', role: 'CATEGORY', is_active: true },
          { id: '3', code: '6010', name: 'Software & Subscriptions', type: 'EXPENSE', role: 'CATEGORY', is_active: true },
        ],
      }
    case 'report.pnl':
      return { income: 1250000, expense: 318000, net: 932000 }
    case 'report.balance_sheet':
      return { assets: 932000, liabilities: 0, equity: 0, net_income: 932000, balanced: true }
    case 'report.cash_flow':
      return { inflow: 1250000, outflow: 318000, net: 932000 }
    case 'account.update': case 'account.set_active': return { ok: true }
    case 'item.list':
      return { items: [
        { id: 'it1', kind: 'SERVICE', name: 'Hourly consulting', default_unit_price: 15000, default_account_id: '2', unit_label: 'hour', is_active: true },
        { id: 'it2', kind: 'EXPENSE', name: 'Cloud hosting', default_unit_price: 5000, default_account_id: '3', unit_label: '', is_active: true },
      ] }
    case 'item.create': return { id: 'item-new' }
    case 'item.set_active': return { ok: true }
    case 'counterparty.list':
      return { counterparties: [
        { id: 'cp1', name: 'Acme Co', kind: 'CUSTOMER' },
        { id: 'cp2', name: 'AWS', kind: 'VENDOR' },
      ] }
    case 'counterparty.create':
      return { id: 'cp-new' }
    case 'invoice.list':
      return { invoices: [
        { id: 'i1', number: 'INV-001', counterparty_name: 'Acme Co', issue_date: '2026-06-01', due_date: '2026-06-30', status: 'OPEN', total: 250000 },
        { id: 'i2', number: 'INV-002', counterparty_name: 'Acme Co', issue_date: '', due_date: '2026-07-15', status: 'DRAFT', total: 80000 },
      ] }
    case 'bill.list':
      return { bills: [
        { id: 'b1', number: 'AWS-07', counterparty_name: 'AWS', issue_date: '2026-06-05', due_date: '2026-07-05', status: 'PARTIAL', total: 12000 },
      ] }
    case 'invoice.get': case 'bill.get':
      return {
        id: args.id || 'i1', number: 'INV-001', counterparty_id: 'cp1', counterparty_name: 'Acme Co',
        issue_date: '2026-06-01', due_date: '2026-06-30', memo: 'June work', status: 'OPEN', total: 250000,
        lines: [
          { id: 'l1', description: 'Design', qty_centi: 100, unit_price: 200000, line_total: 200000, account_id: '2', account_name: 'Consulting Income', is_tax: false },
          { id: 'l2', description: 'Dev', qty_centi: 100, unit_price: 50000, line_total: 50000, account_id: '2', account_name: 'Consulting Income', is_tax: false },
        ],
      }
    case 'invoice.create': case 'bill.create': return { id: 'doc-new' }
    case 'invoice.add_line': case 'bill.add_line': return { line_id: 'line-new' }
    case 'invoice.issue': case 'bill.enter': return { ok: true }
    case 'invoice.update': case 'bill.update': case 'invoice.remove_line': case 'bill.remove_line':
    case 'invoice.void': case 'bill.void': return { ok: true }
    case 'payment.record': return { id: 'pay-new' }
    case 'credit.apply': return { id: 'alloc-new' }
    case 'counterparty.balance':
      // demo: pretend the customer has $600 credit on AR, vendor none
      return { counterparty_id: args.counterparty_id, balance: args.target === 'BILL' ? 0 : -60000,
               credit_available: args.target === 'BILL' ? 0 : 60000 }
    case 'report.ledger':
      return { rows: [
        { entry_id: 'e1', date: '2026-06-01', memo: 'Opening balance', amount: 500000, running: 500000 },
        { entry_id: 'e2', date: '2026-06-10', memo: 'Consulting income', amount: 250000, running: 750000 },
        { entry_id: 'e3', date: '2026-06-15', memo: 'AWS hosting', amount: -12000, running: 738000 },
      ] }
    case 'report.journal':
      return { entries: [
        { entry_id: 'e1', date: '2026-06-10', memo: 'Consulting income', source: 'USER', status: 'POSTED', flow: 'INCOME', amount: 250000 },
        { entry_id: 'e2', date: '2026-06-05', memo: 'AWS hosting', source: 'USER', status: 'POSTED', flow: 'EXPENSE', amount: 12000 },
        { entry_id: 'e3', date: '2026-06-12', memo: 'Payment received — INV-001', source: 'USER', status: 'POSTED', flow: 'OTHER', amount: 250000 },
      ] }
    case 'report.category_txns': {
      const inc = args.type === 'INCOME'
      return {
        type: args.type,
        transactions: inc
          ? [{ entry_id: 'e1', date: '2026-06-10', memo: 'Consulting', category_name: 'Consulting Income', amount: 250000 }]
          : [{ entry_id: 'e2', date: '2026-06-05', memo: 'Hosting', category_name: 'Software & Subscriptions', amount: 12000 }],
        total: inc ? 250000 : 12000,
      }
    }
    case 'book.status': return { onboarded: true, account_count: 3, company_name: 'Acme LLC' }
    case 'book.onboard': return { ok: true }
    case 'book.set_name': return { ok: true }
    case 'app.book_current': return { path: '/Users/you/Library/Application Support/MoneyBooks/Acme_LLC.sqlite', name: 'Acme LLC' }
    case 'app.book_list':
      return { books: [
        { path: '/Users/you/Library/Application Support/MoneyBooks/Acme_LLC.sqlite', name: 'Acme LLC', last_opened: 1781000000, current: true },
        { path: '/Users/you/Library/Application Support/MoneyBooks/Side_Gig.sqlite', name: 'Side Gig', last_opened: 1780000000, current: false },
      ] }
    case 'app.book_create': return { ok: true, path: '/Users/you/Library/Application Support/MoneyBooks/New.sqlite' }
    case 'app.book_open': return { ok: true, path: args.path }
    case 'app.book_forget': return { ok: true }
    case 'mcp.tools': {
      const R = (name, description) => ({ name, description, is_write: false, policy: 'PERMIT' })
      const W = (name, description) => ({ name, description, is_write: true, policy: 'ASK' })
      const tools = [
        R('list_accounts', 'List accounts and categories (optionally filtered).'),
        R('get_account', 'Get one account by id.'),
        R('list_counterparties', 'List clients/vendors.'),
        R('get_invoice', 'Get an invoice with its total and status.'),
        R('list_invoices', 'List all invoices (id, number, counterparty, dates, status, total).'),
        R('get_bill', 'Get a bill with its total and status.'),
        R('list_bills', 'List all bills (id, number, vendor, dates, status, total).'),
        R('report_trial_balance', 'Trial balance as of a date (debits must equal credits).'),
        R('report_pnl', 'Profit & loss over a date range.'),
        R('report_balance_sheet', 'Balance sheet as of a date.'),
        R('report_cash_flow', 'Cash flow over a date range.'),
        W('create_account', 'Create an account or category.'),
        W('record_income', 'Record income: debit a deposit account, credit an income category.'),
        W('record_expense', 'Record an expense: debit an expense category, credit a payment account.'),
        W('post_transaction', 'Post a balanced journal entry (postings sum to zero).'),
        W('create_counterparty', 'Create a client or vendor.'),
        W('create_invoice', 'Create a draft invoice for a counterparty.'),
        W('add_invoice_line', 'Add a line to a draft invoice (tax line supported).'),
        W('issue_invoice', 'Issue a draft invoice (posts Dr AR / Cr income).'),
        W('create_bill', 'Create a draft bill from a vendor.'),
        W('add_bill_line', 'Add a line to a draft bill.'),
        W('enter_bill', 'Enter a draft bill (posts Dr expense / Cr AP).'),
        W('record_payment', 'Record a payment against an invoice or bill.'),
      ]
      return { count: tools.length, tools }
    }
    case 'app.mcp_info':
      return {
        command: '/Users/you/work/money_books/build/money-books-mcp',
        book_path: (args.path && args.path.startsWith('/')) ? args.path
          : '/Users/you/Library/Application Support/MoneyBooks/' + (args.path || 'book.sqlite'),
        config_path: '/Users/you/Library/Application Support/Claude/claude_desktop_config.json',
      }
    default:
      return {}
  }
}
