import { useMemo, useRef, useState } from 'react'
import { Button, DatePicker, Input, Select, Space, Table, Tag, Tooltip, Typography, message } from 'antd'
import type { ColumnsType } from 'antd/es/table'
import { useQuery } from '@tanstack/react-query'
import dayjs, { type Dayjs } from 'dayjs'
import { api, type AuditSession } from '@/lib/api'
import { resizableTableComponents } from '@/components/ResizableTitle'
import { useResizableColumns } from '@/hooks/useResizableColumns'
import { useTableScrollY } from '@/hooks/useTableScrollY'

const resultLabel: Record<string, string> = {
  ok: '成功',
  auth_fail: '鉴权失败',
  capacity_reject: '容量拒绝',
  connect_fail: '连接失败',
  tls_fail: 'TLS 失败',
  cancelled: '已取消',
  unknown: '未知',
}

const modeLabel: Record<string, string> = {
  control: '控制',
  view_only: '只读',
  unknown: '未知',
}

const disconnectLabel: Record<string, string> = {
  user_close: '操作员主动关闭',
  transport_lost: '链路中断',
  auth_fail: '鉴权失败断开',
  host_gone: '被控端离开',
  replaced: '会话被替换',
}

function formatDuration(row: AuditSession): string {
  if (!row.openedAt || !row.closedAt) return '—'
  const ms = dayjs(row.closedAt).diff(dayjs(row.openedAt))
  if (ms < 0) return '—'
  const s = Math.floor(ms / 1000)
  if (s < 60) return `${s}s`
  const m = Math.floor(s / 60)
  const rem = s % 60
  if (m < 60) return `${m}m ${rem}s`
  const h = Math.floor(m / 60)
  return `${h}h ${m % 60}m`
}

/** Human-readable 备注 from session fields (list + tooltip). */
function formatRemark(row: AuditSession): string {
  const parts: string[] = []

  switch (row.result) {
    case 'ok':
      if (row.openedAt && row.closedAt) {
        if (row.disconnectReason === 'user_close') {
          parts.push('远控已接通并正常结束（操作员主动关闭会话）')
        } else if (row.disconnectReason === 'transport_lost') {
          parts.push('远控已接通，后因网络/进程中断结束（非主动关闭）')
        } else if (disconnectLabel[row.disconnectReason]) {
          parts.push(`远控已接通并结束：${disconnectLabel[row.disconnectReason]}`)
        } else {
          parts.push('远控已接通并结束')
        }
      } else if (row.openedAt) {
        parts.push('远控已接通；关闭事件尚未上报（可能仍在线，或仅有部分上报）')
      } else {
        parts.push('已发起连接，尚未确认媒体面接通')
      }
      break
    case 'auth_fail':
      parts.push('未能建立远控：媒体口令/鉴权失败（请核对 ROAD_DESK_PSK 与被控端一致）')
      break
    case 'capacity_reject':
      parts.push('未能建立远控：被控端已达并发上限（默认最多 8 路 Viewer）')
      break
    case 'connect_fail':
      parts.push('未能建立远控：TCP/媒体连接失败（主机不可达、端口未开或被防火墙拦截）')
      break
    case 'tls_fail':
      parts.push(
        '未能建立远控：TLS 握手或证书指纹校验失败（检查指纹或调试用 ROAD_DESK_TLS_INSECURE）',
      )
      break
    case 'cancelled':
      parts.push('连接尝试已取消')
      break
    default:
      parts.push(
        row.openedAt
          ? '会话状态不完整，请结合时间与端点判断'
          : '结果未知或上报尚未归并完成',
      )
      break
  }

  if (row.mode === 'view_only') {
    parts.push('模式：只读（不向被控端注入键鼠）')
  } else if (row.mode === 'control' && row.openedAt) {
    parts.push('模式：控制')
  }

  if (row.agentEndpoint) {
    parts.push(`媒体目标 ${row.agentEndpoint}`)
  }

  const extras: string[] = []
  if (row.usedClipboard) extras.push('剪贴板')
  if (row.usedFileTransfer) {
    const ft = formatFileTransfer(row)
    extras.push(ft === '是' ? '文件传输' : `文件传输（${ft}）`)
    const detail = formatFileTransferDetail(row)
    if (detail && detail !== ft && detail !== '是') {
      parts.push(`文件明细：${detail.replace(/\n\n/g, '；').replace(/\n/g, ' ')}`)
    }
  }
  if (extras.length) {
    parts.push(`曾使用：${extras.join('、')}`)
  }

  const meta = row.meta && typeof row.meta === 'object' ? row.meta : {}
  const reconnect = meta.reconnectCount
  if (typeof reconnect === 'number' && reconnect > 0) {
    parts.push(`期间自动重连 ${reconnect} 次`)
  }

  if (row.partial) {
    parts.push('标记为部分上报（仅单侧事件，另一端尚未归并）')
  }

  if (!parts.length) return ''
  return parts.join('。') + '。'
}

type FileTransferItem = {
  dir?: string
  name?: string
  path?: string
  isDir?: boolean
}

type FileTransferMeta = {
  out?: boolean
  in?: boolean
  outCount?: number
  inCount?: number
  outEntries?: number
  inEntries?: number
  items?: FileTransferItem[]
}

/** Viewer→Host / Host→Viewer: counts + top-level names (dir → name only, not children). */
function formatFileTransfer(row: AuditSession): string {
  if (!row.usedFileTransfer) return '—'
  const meta = row.meta && typeof row.meta === 'object' ? row.meta : {}
  const ft = meta.fileTransfer as FileTransferMeta | undefined
  if (!ft || typeof ft !== 'object') return '是'
  const outN = typeof ft.outCount === 'number' ? ft.outCount : ft.out ? 1 : 0
  const inN = typeof ft.inCount === 'number' ? ft.inCount : ft.in ? 1 : 0
  const outE = typeof ft.outEntries === 'number' ? ft.outEntries : 0
  const inE = typeof ft.inEntries === 'number' ? ft.inEntries : 0
  const parts: string[] = []
  if (outN > 0) {
    parts.push(outE > 0 ? `发${outN}次/${outE}项` : `发${outN}次`)
  }
  if (inN > 0) {
    parts.push(inE > 0 ? `收${inN}次/${inE}项` : `收${inN}次`)
  }
  const items = Array.isArray(ft.items) ? ft.items : []
  if (items.length) {
    const labels = items.slice(0, 8).map((it) => {
      const arrow = it.dir === 'in' ? '←' : '→'
      const kind = it.isDir ? '目录' : '文件'
      const name = (it.name || '').trim() || '(未命名)'
      return `${arrow}${kind}:${name}`
    })
    parts.push(labels.join(' '))
    if (items.length > 8) parts.push(`…+${items.length - 8}`)
  }
  return parts.length ? parts.join(' · ') : '是'
}

function formatFileTransferDetail(row: AuditSession): string {
  const meta = row.meta && typeof row.meta === 'object' ? row.meta : {}
  const ft = meta.fileTransfer as FileTransferMeta | undefined
  const items = ft && Array.isArray(ft.items) ? ft.items : []
  if (!items.length) return formatFileTransfer(row)
  return items
    .map((it) => {
      const arrow = it.dir === 'in' ? '收' : '发'
      const kind = it.isDir ? '目录' : '文件'
      const name = (it.name || '').trim() || '(未命名)'
      const path = (it.path || '').trim()
      return path ? `${arrow} ${kind} ${name}\n${path}` : `${arrow} ${kind} ${name}`
    })
    .join('\n\n')
}

export function AuditPage() {
  const tableWrapRef = useRef<HTMLDivElement>(null)
  const [range, setRange] = useState<[Dayjs | null, Dayjs | null] | null>(null)
  const [operator, setOperator] = useState('')
  const [agentId, setAgentId] = useState('')
  const [result, setResult] = useState<string | undefined>()
  const [departmentId, setDepartmentId] = useState<number | undefined>()
  const [exporting, setExporting] = useState(false)

  const filterParams = {
    from: range?.[0]?.startOf('day').toISOString(),
    to: range?.[1]?.endOf('day').toISOString(),
    operator: operator.trim() || undefined,
    agent_id: agentId.trim() || undefined,
    result,
    department_id: departmentId,
    limit: 100,
  }

  const deptsQ = useQuery({ queryKey: ['departments'], queryFn: api.departments })

  const listQ = useQuery({
    queryKey: ['audit-sessions', filterParams],
    queryFn: () => api.auditSessions(filterParams),
  })

  const tableScrollY = useTableScrollY(tableWrapRef, [listQ.isLoading, listQ.data?.items?.length])

  const columns = useMemo<ColumnsType<AuditSession>>(
    () => [
      {
        title: '时间',
        dataIndex: 'attemptedAt',
        width: 170,
        render: (v: string) => dayjs(v).format('YYYY-MM-DD HH:mm:ss'),
      },
      { title: '操作员', dataIndex: 'operatorName', width: 100, ellipsis: true },
      {
        title: '部门',
        dataIndex: 'departmentName',
        width: 100,
        ellipsis: true,
        render: (v: string) => v || '—',
      },
      {
        title: '发起端',
        key: 'viewer',
        width: 150,
        ellipsis: true,
        render: (_, r) => {
          const host = (r.viewerHost || '').trim()
          const ip = (r.viewerIp || '').trim()
          if (!host && !ip) return '—'
          const tip = [host && `本机 ${host}`, ip && `IP ${ip}`].filter(Boolean).join(' · ')
          return (
            <Tooltip title={tip} placement="topLeft">
              <div className="min-w-0 leading-tight">
                <div className="truncate">{host || '—'}</div>
                <Typography.Text type="secondary" className="block truncate text-xs">
                  {ip || '—'}
                </Typography.Text>
              </div>
            </Tooltip>
          )
        },
      },
      {
        title: '被控端',
        key: 'agent',
        width: 140,
        ellipsis: true,
        render: (_, r) => r.agentName || r.agentId || '—',
      },
      {
        title: '结果',
        dataIndex: 'result',
        width: 96,
        render: (v: string) => {
          const color = v === 'ok' ? 'success' : v === 'unknown' ? 'default' : 'error'
          return <Tag color={color}>{resultLabel[v] || v}</Tag>
        },
      },
      {
        title: '模式',
        dataIndex: 'mode',
        width: 72,
        render: (v: string) => modeLabel[v] || v,
      },
      {
        title: '时长',
        key: 'dur',
        width: 80,
        render: (_, r) => formatDuration(r),
      },
      {
        title: '备注',
        key: 'remark',
        ellipsis: true,
        render: (_, r) => {
          const text = formatRemark(r)
          return (
            <Tooltip
              title={<div className="max-w-md whitespace-pre-wrap text-xs">{text}</div>}
              placement="topLeft"
            >
              <Typography.Text ellipsis className="max-w-full">
                {text || '—'}
              </Typography.Text>
            </Tooltip>
          )
        },
      },
      {
        title: '剪贴板',
        dataIndex: 'usedClipboard',
        width: 64,
        align: 'center',
        render: (v: boolean) => (v ? '是' : '—'),
      },
      {
        title: '文件',
        key: 'file',
        width: 200,
        ellipsis: true,
        render: (_, r) => {
          const text = formatFileTransfer(r)
          if (text === '—') return '—'
          return (
            <Tooltip
              title={<div className="max-w-lg whitespace-pre-wrap text-xs">{formatFileTransferDetail(r)}</div>}
              placement="topLeft"
            >
              <span className="truncate">{text}</span>
            </Tooltip>
          )
        },
      },
      {
        title: '',
        key: 'partial',
        width: 64,
        render: (_, r) => (r.partial ? <Tag>部分</Tag> : null),
      },
    ],
    [],
  )

  const resizableColumns = useResizableColumns(columns)

  const onExport = async () => {
    setExporting(true)
    try {
      await api.downloadAuditCsv({
        from: filterParams.from,
        to: filterParams.to,
        operator: filterParams.operator,
        agent_id: filterParams.agent_id,
        result: filterParams.result,
        department_id: filterParams.department_id,
      })
      message.success('已开始下载 CSV')
    } catch (e) {
      message.error(e instanceof Error ? e.message : '导出失败')
    } finally {
      setExporting(false)
    }
  }

  return (
    <div className="flex h-full flex-col gap-3 p-4">
      <Space wrap>
        <DatePicker.RangePicker value={range} onChange={(v) => setRange(v)} allowClear />
        <Input
          allowClear
          placeholder="操作员"
          value={operator}
          onChange={(e) => setOperator(e.target.value)}
          style={{ width: 140 }}
        />
        <Select
          allowClear
          placeholder="部门"
          style={{ width: 160 }}
          value={departmentId}
          onChange={setDepartmentId}
          loading={deptsQ.isLoading}
          options={(deptsQ.data ?? []).map((d) => ({ value: d.id, label: d.name }))}
        />
        <Input
          allowClear
          placeholder="被控端 ID"
          value={agentId}
          onChange={(e) => setAgentId(e.target.value)}
          style={{ width: 200 }}
        />
        <Select
          allowClear
          placeholder="结果"
          style={{ width: 140 }}
          value={result}
          onChange={setResult}
          options={Object.entries(resultLabel).map(([value, label]) => ({ value, label }))}
        />
        <Button type="primary" loading={exporting} onClick={onExport}>
          导出 CSV
        </Button>
      </Space>
      <div ref={tableWrapRef} className="min-h-0 flex-1">
        <Table<AuditSession>
          size="small"
          rowKey="id"
          loading={listQ.isLoading}
          dataSource={listQ.data?.items ?? []}
          columns={resizableColumns}
          components={resizableTableComponents}
          pagination={false}
          scroll={{ y: tableScrollY, x: 1600 }}
        />
      </div>
    </div>
  )
}
