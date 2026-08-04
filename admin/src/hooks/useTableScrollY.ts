import { useEffect, useState, type RefObject } from 'react'

/**
 * Measure a container and compute antd Table body scroll.y so the table fills
 * available height (header + pagination stay outside the scroll area).
 */
export function useTableScrollY(
  containerRef: RefObject<HTMLElement | null>,
  deps: unknown[] = [],
  extraOffset = 0,
): number {
  const [scrollY, setScrollY] = useState(400)

  useEffect(() => {
    const el = containerRef.current
    if (!el) return

    let raf = 0
    const measure = () => {
      cancelAnimationFrame(raf)
      raf = requestAnimationFrame(() => {
        const header =
          el.querySelector<HTMLElement>('.ant-table-header') ??
          el.querySelector<HTMLElement>('.ant-table-thead')
        const pagination = el.querySelector<HTMLElement>('.ant-table-pagination')
        const headerH = header?.offsetHeight ?? 55
        const paginationH = pagination
          ? pagination.offsetHeight +
            parseFloat(getComputedStyle(pagination).marginTop || '0') +
            parseFloat(getComputedStyle(pagination).marginBottom || '0')
          : 0
        const next = Math.max(120, el.clientHeight - headerH - paginationH - extraOffset)
        setScrollY((prev) => (prev === next ? prev : next))
      })
    }

    measure()
    const ro = new ResizeObserver(measure)
    ro.observe(el)
    window.addEventListener('resize', measure)
    return () => {
      cancelAnimationFrame(raf)
      ro.disconnect()
      window.removeEventListener('resize', measure)
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [containerRef, extraOffset, ...deps])

  return scrollY
}
