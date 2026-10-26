// Icons of Coinshift's own pages (components/Icon.svelte): line icons on a 24 grid.
export const LOCAL_ICONS = {
  /** Swaps: two arrows the opposite way. */
  swap: 'M4 8h14M14 4l4 4-4 4M20 16H6M10 12l-4 4 4 4',
  /** A new swap: arrows and a plus. */
  offer: 'M4 8h11M11 4l4 4-4 4M14 16H6M10 12l-4 4 4 4M19 13v6M16 16h6',
  /** L1: a chain of two links. */
  l1: 'M9.5 14.5l5-5M8 11l-2 2a3.5 3.5 0 0 0 5 5l2-2M16 13l2-2a3.5 3.5 0 0 0-5-5l-2 2',
} as const satisfies Record<string, string>;
