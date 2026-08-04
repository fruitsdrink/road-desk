import type { HTMLAttributes, SyntheticEvent } from 'react'
import { Resizable, type ResizeCallbackData } from 'react-resizable'
import 'react-resizable/css/styles.css'

export type ResizableTitleProps = HTMLAttributes<HTMLTableCellElement> & {
  onResize?: (e: SyntheticEvent, data: ResizeCallbackData) => void
  width?: number
}

/** antd Table header cell with drag-to-resize (official pattern via react-resizable). */
export function ResizableTitle({ onResize, width, ...rest }: ResizableTitleProps) {
  if (width == null || onResize == null) {
    return <th {...rest} />
  }

  return (
    <Resizable
      width={width}
      height={0}
      handle={
        <span
          className="react-resizable-handle"
          onClick={(e) => e.stopPropagation()}
        />
      }
      onResize={onResize}
      draggableOpts={{ enableUserSelectHack: false }}
      minConstraints={[60, 0]}
    >
      <th {...rest} />
    </Resizable>
  )
}

export const resizableTableComponents = {
  header: { cell: ResizableTitle },
}
