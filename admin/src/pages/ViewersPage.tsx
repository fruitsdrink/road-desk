import { useMemo, useRef, useState } from 'react'
import { Switch, Table, Tag } from 'antd'
import type { ColumnsType } from 'antd/es/table'
import { useQuery } from '@tanstack/react-query'
import { api, type ViewerPresence } from '@/lib/api'
import { resizableTableComponents } from '@/components/ResizableTitle'
import { useResizableColumns } from '@/hooks/useResizableColumns'
import { useTableScrollY } from '@/hooks/useTableScrollY'

function formatTime(iso: string | null) {
  if (!iso) return '—'
  const d = new Date(iso)
  if (Number.isNaN(d.getTime())) return iso
  return d.toLocaleString()
}

export function ViewersPage() {
  const tableWrapRef = useRef<HTMLDivElement>(null)
  const [showOffline, setShowOffline] = useState(false)
  const viewersQ = useQuery({
    queryKey: ['viewers'],
    queryFn: api.viewers,
    refetchInterval: 5000,
  })

  const all = viewersQ.data ?? []
  const onlineCount = useMemo(() => all.filter((v) => v.online).length, [all])
  const rows = useMemo(
    () => (showOffline ? all : all.filter((v) => v.online)),
    [all, showOffline],
  )
  const tableScrollY = useTableScrollY(tableWrapRef, [
    viewersQ.isLoading,
    rows.length,
    showOffline,
  ])

  const columns = useMemo<ColumnsType<ViewerPresence>>(
    () => [
      {
        title: '主机名',
        dataIndex: 'hostname',
        ellipsis: true,
        render: (v: string) => v || '—',
      },
      {
        title: '用户',
        dataIndex: 'username',
        width: 140,
        ellipsis: true,
        render: (v: string) => v || '—',
      },
      {
        title: '认证',
        dataIndex: 'authMode',
        width: 100,
        render: (v: string) =>
          v === 'account' ? <Tag color="blue">账号</Tag> : <Tag>PSK</Tag>,
      },
      {
        title: '版本',
        dataIndex: 'version',
        width: 100,
        render: (v: string) => v || '—',
      },
      {
        title: 'IP',
        dataIndex: 'clientIp',
        width: 140,
        render: (v: string) => v || '—',
      },
      {
        title: '状态',
        dataIndex: 'online',
        width: 90,
        render: (online: boolean) =>
          online ? <Tag color="green">在线</Tag> : <Tag>离线</Tag>,
      },
      {
        title: '最后在线',
        dataIndex: 'lastSeenAt',
        width: 180,
        render: (v: string | null) => formatTime(v),
      },
    ],
    [],
  )

  const resizableColumns = useResizableColumns(columns)

  return (
    <div className="flex h-full min-h-0 flex-col gap-3 p-4">
      <div className="flex shrink-0 items-center justify-between gap-3">
        <div className="text-base font-medium text-slate-800">操作端（Viewer）</div>
        <div className="flex items-center gap-4 text-sm text-slate-500">
          <label className="flex cursor-pointer items-center gap-2">
            <Switch size="small" checked={showOffline} onChange={setShowOffline} />
            显示离线
          </label>
          <span>
            在线 {onlineCount}
            {showOffline ? ` / 共 ${all.length}` : ''}
          </span>
        </div>
      </div>
      <div ref={tableWrapRef} className="min-h-0 flex-1">
        <Table
          className="admin-table-fill"
          rowKey="viewerId"
          loading={viewersQ.isLoading}
          dataSource={rows}
          columns={resizableColumns}
          components={resizableTableComponents}
          scroll={{ x: 900, y: tableScrollY }}
          tableLayout="fixed"
          pagination={{
            showSizeChanger: true,
            showTotal: (total) => `共 ${total} 条`,
          }}
        />
      </div>
    </div>
  )
}
