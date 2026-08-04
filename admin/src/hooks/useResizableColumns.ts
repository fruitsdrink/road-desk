import { useCallback, useMemo, useState, type SyntheticEvent } from 'react'
import type { ColumnsType, ColumnType } from 'antd/es/table'
import type { ResizeCallbackData } from 'react-resizable'

function columnKey<T>(col: ColumnType<T>, index: number): string {
  if (col.key != null) return String(col.key)
  if (col.dataIndex != null) {
    return Array.isArray(col.dataIndex) ? col.dataIndex.join('.') : String(col.dataIndex)
  }
  return String(index)
}

/** Merge column definitions with drag-resizable widths (antd Table + react-resizable). */
export function useResizableColumns<T>(columns: ColumnsType<T>): ColumnsType<T> {
  const [widths, setWidths] = useState<Record<string, number>>({})

  const handleResize = useCallback(
    (key: string) =>
      (_: SyntheticEvent, { size }: ResizeCallbackData) => {
        setWidths((prev) => ({ ...prev, [key]: size.width }))
      },
    [],
  )

  return useMemo(
    () =>
      columns.map((col, index) => {
        if (!('dataIndex' in col) && !('key' in col) && !('title' in col)) {
          return col
        }
        const c = col as ColumnType<T>
        const key = columnKey(c, index)
        const width = widths[key] ?? (typeof c.width === 'number' ? c.width : undefined)
        if (width == null) return c

        return {
          ...c,
          width,
          onHeaderCell: () => ({
            width,
            onResize: handleResize(key),
          }),
        }
      }),
    [columns, widths, handleResize],
  )
}
