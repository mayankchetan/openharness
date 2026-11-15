import { describe, expect, it, vi } from 'vitest'
import type { RegisteredSession } from '../lib/registry.js'
import {
  RUNNING_MAX, RUNNING_PAGE_SIZE, RunningOrder, isRunning, runningPage,
  type RankFn, type RunningPageRow, type RunningRank,
} from './runningAgents.js'

const row = (over: Partial<RegisteredSession> = {}): RegisteredSession =>
  ({ agentId: 'a1', engine: 'claude', active: true, ...over }) as RegisteredSession

describe('isRunning', () => {
  it('keeps an active row', () => {
    expect(isRunning(row())).toBe(true)
  })

  it.each([
    ['identityHold', { identityHold: 'lost' }],
    ['admissionHold', { admissionHold: 'pending' }],
    ['interpretationHold', { interpretationHold: 'tail' }],
    ['closePlan', { closePlan: { state: 'waiting' } }],
    ['active false', { active: false }],
  ])('drops a row with %s', (_name, over) => {
    expect(isRunning(row(over as Partial<RegisteredSession>))).toBe(false)
  })

  it("drops a cloud engine row and keeps engine 'terminal'", () => {
    expect(isRunning(row({ engine: 'devin-cloud' as RegisteredSession['engine'] }))).toBe(false)
    expect(isRunning(row({ engine: 'terminal' as RegisteredSession['engine'], active: true }))).toBe(true)
  })
})

/** table[id] = activity (or a full rank). Records every call's ids. */
function ranker(table: Record<string, number | RunningRank>) {
  const calls: string[][] = []
  const rank: RankFn = async (ids) => {
    calls.push([...ids])
    return new Map(ids.map(id => {
      const v = table[id]
      return [id, typeof v === 'number' ? { activity: v, openedAt: 0 } : v ?? { activity: 0, openedAt: 0 }]
    }))
  }
  return { rank, calls }
}
const cands = (n: number) => Array.from({ length: n }, (_, i) => ({ id: `a${String(i).padStart(2, '0')}`, registeredAt: 1000 + i }))
const activityByIndex = (n: number) => Object.fromEntries(Array.from({ length: n }, (_, i) => [`a${String(i).padStart(2, '0')}`, i]))

describe('RunningOrder', () => {
  it('first build takes the newest 16', async () => {
    const order = new RunningOrder()
    const { rank, calls } = ranker(activityByIndex(20))
    const out = await order.reconcile(cands(20), rank)
    expect(RUNNING_MAX).toBe(16)
    expect(out).toHaveLength(16)
    expect(out[0]).toBe('a19')
    expect(out[15]).toBe('a04')
    expect(calls).toHaveLength(1)
    expect(calls[0]).toHaveLength(20)
  })

  it('a finished turn does not move an agent', async () => {
    const order = new RunningOrder()
    const first = ranker(activityByIndex(10))
    const a = await order.reconcile(cands(10), first.rank)
    const second = ranker({ ...activityByIndex(10), a00: 999 })
    const b = await order.reconcile(cands(10), second.rank)
    expect(b).toEqual(a)
    expect(second.calls).toHaveLength(0)
    expect(first.calls).toHaveLength(1)
  })

  it('a new agent goes first and the end drops', async () => {
    const order = new RunningOrder()
    const { rank, calls } = ranker(activityByIndex(20))
    const first = await order.reconcile(cands(16), rank)
    const withNew = [...cands(16), { id: 'new1', registeredAt: 5000 }]
    const out = await order.reconcile(withNew, rank)
    expect(out).toEqual(['new1', ...first.slice(0, 15)])
    expect(calls).toHaveLength(1)
    // The cut agent is known: it never comes back at the front, only as a fill.
    const cut = first[15]
    expect(await order.reconcile(withNew, rank)).toEqual(out)
    const filled = await order.reconcile(withNew.filter(c => c.id !== 'a10'), rank)
    expect(filled[0]).toBe('new1')
    expect(filled[filled.length - 1]).toBe(cut)
  })

  it('several new agents go first, newest registeredAt first', async () => {
    const order = new RunningOrder()
    const { rank } = ranker(activityByIndex(3))
    const first = await order.reconcile(cands(3), rank)
    const out = await order.reconcile([...cands(3), { id: 'n1', registeredAt: 7000 }, { id: 'n2', registeredAt: 8000 }], rank)
    expect(out).toEqual(['n2', 'n1', ...first])
  })

  it('a stopped agent drops out and the best unlisted fills the end', async () => {
    const order = new RunningOrder()
    const { rank, calls } = ranker(activityByIndex(20))
    const first = await order.reconcile(cands(20), rank)
    expect(first).toHaveLength(16)
    const unlisted = cands(20).map(c => c.id).filter(id => !first.includes(id))
    expect(unlisted).toHaveLength(4)
    const stopped = first[5]
    const out = await order.reconcile(cands(20).filter(c => c.id !== stopped), rank)
    expect(out).toHaveLength(16)
    expect(out).not.toContain(stopped)
    expect(out.slice(0, 5)).toEqual(first.slice(0, 5))
    expect(out[15]).toBe('a03')
    expect(calls).toHaveLength(2)
    expect(calls[1].slice().sort()).toEqual(unlisted.slice().sort())
  })

  it('breaks ties by openedAt, then by id', async () => {
    const order = new RunningOrder()
    const { rank } = ranker({
      b: { activity: 5, openedAt: 1 }, c: { activity: 5, openedAt: 9 },
      d: { activity: 5, openedAt: 9 }, a: { activity: 5, openedAt: 1 },
    })
    const out = await order.reconcile(['a', 'b', 'c', 'd'].map((id, i) => ({ id, registeredAt: i })), rank)
    expect(out).toEqual(['c', 'd', 'a', 'b'])
  })

  it('pin puts an unlisted running agent first, cuts the last, and refuses an unknown id', async () => {
    const order = new RunningOrder()
    const { rank } = ranker(activityByIndex(20))
    const first = await order.reconcile(cands(20), rank)
    const outside = 'a00'
    expect(first).not.toContain(outside)
    expect(order.knows(outside)).toBe(true)
    expect(order.pin('zzz')).toBe(false)
    expect(order.pin(outside)).toBe(true)
    const out = await order.reconcile(cands(20), rank)
    expect(out).toEqual([outside, ...first.slice(0, 15)])
  })

  it('pin of an agent already listed keeps the order', async () => {
    const order = new RunningOrder()
    const { rank } = ranker(activityByIndex(5))
    const first = await order.reconcile(cands(5), rank)
    expect(order.pin(first[3])).toBe(true)
    expect(await order.reconcile(cands(5), rank)).toEqual(first)
  })

  it('pin survives later reconciles', async () => {
    const order = new RunningOrder()
    const { rank } = ranker(activityByIndex(20))
    await order.reconcile(cands(20), rank)
    order.pin('a00')
    const a = await order.reconcile(cands(20), rank)
    const b = await order.reconcile(cands(20), rank)
    expect(a[0]).toBe('a00')
    expect(b).toEqual(a)
  })

  it('reset forgets order, known set and pin', async () => {
    const order = new RunningOrder()
    const { rank, calls } = ranker(activityByIndex(20))
    await order.reconcile(cands(20), rank)
    order.pin('a00')
    order.reset()
    expect(order.knows('a19')).toBe(false)
    expect(order.pin('a00')).toBe(false)
    const out = await order.reconcile(cands(20), rank)
    expect(out[0]).toBe('a19')
    expect(out).not.toContain('a00')
    expect(calls).toHaveLength(2)
  })

  it('does not call rank for an empty running set', async () => {
    const order = new RunningOrder()
    const rank = vi.fn(async () => new Map<string, RunningRank>())
    expect(await order.reconcile([], rank)).toEqual([])
    expect(rank).not.toHaveBeenCalled()
  })

  it('honours a smaller max', async () => {
    const order = new RunningOrder(2)
    const { rank } = ranker(activityByIndex(5))
    expect(await order.reconcile(cands(5), rank)).toEqual(['a04', 'a03'])
  })
})

describe('runningPage', () => {
  const rows = (n: number): RunningPageRow[] => Array.from({ length: n }, (_, i) => ({
    id: `id${i}`, name: `n${i}`, engine: 'claude', machineId: 'm', status: 'idle' as const,
  }))

  it('clamps a high offset to the last full page', () => {
    expect(RUNNING_PAGE_SIZE).toBe(4)
    const page = runningPage(rows(10), 999)
    expect(page.offset).toBe(6)
    expect(page.total).toBe(10)
    expect(page.rows.map(r => r.id)).toEqual(['id6', 'id7', 'id8', 'id9'])
  })

  it('clamps a negative offset to 0 and slices a page', () => {
    const page = runningPage(rows(10), -1)
    expect(page.offset).toBe(0)
    expect(page.rows).toHaveLength(4)
  })

  it('gives an empty page for no rows', () => {
    expect(runningPage([], 5)).toEqual({ offset: 0, total: 0, rows: [] })
  })

  it('gives a short list as one page from 0', () => {
    expect(runningPage(rows(3), 2)).toMatchObject({ offset: 0, total: 3 })
  })
})

describe('RunningOrder: two reconciles at once', () => {
  // The pin of a running agent and a roster sync can both reach reconcile, and rank awaits I/O: one in flight at a time, so the second sees what the first built.
  it('serializes: two calls started together give what the same two calls in a row give, and rank never runs twice at once', async () => {
    const make = () => {
      const order = new RunningOrder(4)
      let inFlight = 0
      let overlap = 0
      const rank: RankFn = async (list) => {
        inFlight++
        overlap = Math.max(overlap, inFlight)
        await new Promise(r => setTimeout(r, 5))
        inFlight--
        return new Map(list.map(id => [id, { activity: 100 - Number(id.slice(1)), openedAt: 0 }]))   // a0 is the newest
      }
      return { order, rank, overlap: () => overlap }
    }
    const row = (id: string, i: number) => ({ id, registeredAt: 1000 + i })
    const five = ['a0', 'a1', 'a2', 'a3', 'a4'].map(row)
    const three = ['a2', 'a3', 'a4'].map(row)   // a0 and a1 stopped: two kept, one refilled from the unlisted

    const serial = make()
    expect(await serial.order.reconcile(five, serial.rank)).toEqual(['a0', 'a1', 'a2', 'a3'])
    const expected = [await serial.order.reconcile(three, serial.rank), await serial.order.reconcile(three, serial.rank)]
    expect(expected).toEqual([['a2', 'a3', 'a4'], ['a2', 'a3', 'a4']])

    const both = make()
    await both.order.reconcile(five, both.rank)
    const got = await Promise.all([both.order.reconcile(three, both.rank), both.order.reconcile(three, both.rank)])
    expect(got).toEqual(expected)   // unserialized, the first came back one tile short: [a2, a3]
    expect(both.overlap()).toBe(1)
  })

  it('a failing call does not block the next one', async () => {
    const order = new RunningOrder(4)
    const boom: RankFn = async () => { throw new Error('rank failed') }
    const ok: RankFn = async (list) => new Map(list.map(id => [id, { activity: 1, openedAt: 0 }]))
    const running = [{ id: 'a0', registeredAt: 1 }]
    const [first, second] = await Promise.allSettled([order.reconcile(running, boom), order.reconcile(running, ok)])
    expect(first.status).toBe('rejected')
    expect(second).toEqual({ status: 'fulfilled', value: ['a0'] })
  })
})
