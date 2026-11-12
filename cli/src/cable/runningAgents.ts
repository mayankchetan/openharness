// Which agents the dial lists when no desktop window is open, and in what order. Pure: no I/O, no clock.
//
// The list is at most RUNNING_MAX long (the dial's tile cap). Rank decides WHICH agents make the first
// list and which ones fill a gap; after that the order is stable, so a finished turn never moves a tile
// under the finger. A new agent goes to the front, an agent that stops drops out, and the end refills.
import type { RegisteredSession } from '../lib/registry.js'
import { isCloudAgent } from '../lib/cloudAgents.js'

/** Equals CABLE_MAX_AGENTS in the dial firmware (cable_client.h). */
export const RUNNING_MAX = 16
/** Rows on one page of the "All agents" list; equals CABLE_RUNNING_ROWS in the firmware. */
export const RUNNING_PAGE_SIZE = 4

export type RunningStatus = 'working' | 'question' | 'idle'

export interface RunningRank { activity: number; openedAt: number }
export type RankFn = (ids: string[]) => Promise<Map<string, RunningRank>>

/**
 * A live agent worth a tile: active, with no identity, admission or interpretation hold and no close
 * plan. Plain terminals count. Cloud rows do not (they run elsewhere).
 */
export function isRunning(s: RegisteredSession): boolean {
  return s.active === true && !s.identityHold && !s.admissionHold && !s.interpretationHold && !s.closePlan &&
    !isCloudAgent(s.engine)
}

const NO_RANK: RunningRank = { activity: 0, openedAt: 0 }

export class RunningOrder {
  private order: string[] = []
  private known = new Set<string>()

  constructor(private readonly max: number = RUNNING_MAX) {}

  /**
   * The ids to list now. `running` is every running agent. The first call after a reset ranks them all;
   * later calls keep the current order and only place what changed.
   */
  async reconcile(running: Array<{ id: string; registeredAt: number }>, rank: RankFn): Promise<string[]> {
    const now = new Set(running.map(r => r.id))
    if (this.order.length === 0) {
      this.order = (await this.ranked([...now], rank)).slice(0, this.max)
    } else {
      const kept = this.order.filter(id => now.has(id))
      const fresh = running
        .filter(r => !this.known.has(r.id))
        .sort((a, b) => b.registeredAt - a.registeredAt || (a.id < b.id ? -1 : a.id > b.id ? 1 : 0))
        .map(r => r.id)
      this.order = [...fresh, ...kept].slice(0, this.max)
      if (this.order.length < this.max) {
        const listed = new Set(this.order)
        const unlisted = [...now].filter(id => !listed.has(id))
        if (unlisted.length) this.order.push(...(await this.ranked(unlisted, rank)).slice(0, this.max - this.order.length))
      }
    }
    this.known = now
    return [...this.order]
  }

  /**
   * Put a running agent at the front and drop the last one when the list is full. An agent that is
   * already listed stays where it is. False when the id is not in the last reconcile's running set.
   */
  pin(id: string): boolean {
    if (!this.known.has(id)) return false
    if (this.order.includes(id)) return true
    this.order = [id, ...this.order].slice(0, this.max)
    return true
  }

  knows(id: string): boolean {
    return this.known.has(id)
  }

  /** Forget the order, the known set and every pin. */
  reset(): void {
    this.order = []
    this.known = new Set()
  }

  private ranked(ids: string[], rank: RankFn): Promise<string[]> {
    return sortByRank(ids, rank)
  }
}

/** The ids newest first: activity, then openedAt, both descending, then id ascending. No stability rule. */
export async function sortByRank(ids: string[], rank: RankFn): Promise<string[]> {
  if (ids.length === 0) return []
  const ranks = await rank(ids)
  const of = (id: string) => ranks.get(id) ?? NO_RANK
  return [...ids].sort((a, b) =>
    of(b).activity - of(a).activity || of(b).openedAt - of(a).openedAt || (a < b ? -1 : a > b ? 1 : 0))
}

export interface RunningPageRow { id: string; name: string; engine: string; machineId: string; status: RunningStatus }

/** One page, clamped like the Pro's library page: the offset never runs past the last full page. */
export function runningPage(rows: RunningPageRow[], offset: number): { offset: number; total: number; rows: RunningPageRow[] } {
  const start = Math.max(0, Math.min(offset, Math.max(0, rows.length - RUNNING_PAGE_SIZE)))
  return { offset: start, total: rows.length, rows: rows.slice(start, start + RUNNING_PAGE_SIZE) }
}
