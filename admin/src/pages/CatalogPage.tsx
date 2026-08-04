import { useMemo, useRef, useState } from 'react'
import {
  Button,
  Drawer,
  Form,
  Input,
  Layout,
  Modal,
  Select,
  Space,
  Table,
  Tag,
  Tooltip,
  Tree,
  message,
} from 'antd'
import {
  AppstoreOutlined,
  DeleteOutlined,
  FolderAddOutlined,
} from '@ant-design/icons'
import type { ColumnsType } from 'antd/es/table'
import type { DataNode } from 'antd/es/tree'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { api, type Agent, type Group } from '@/lib/api'
import { resizableTableComponents } from '@/components/ResizableTitle'
import { useResizableColumns } from '@/hooks/useResizableColumns'
import { useTableScrollY } from '@/hooks/useTableScrollY'

const { Sider, Content } = Layout

function buildTree(groups: Group[]): DataNode[] {
  const byParent = new Map<number | null, Group[]>()
  for (const g of groups) {
    const key = g.parentId
    const list = byParent.get(key) ?? []
    list.push(g)
    byParent.set(key, list)
  }
  const walk = (parentId: number | null): DataNode[] =>
    (byParent.get(parentId) ?? []).map((g) => ({
      key: String(g.id),
      title: g.name,
      children: walk(g.id),
    }))
  return walk(null)
}

export function CatalogPage() {
  const qc = useQueryClient()
  const [selectedGroupId, setSelectedGroupId] = useState<number | null>(null)
  const [editing, setEditing] = useState<Agent | null>(null)
  const [tagModal, setTagModal] = useState(false)
  const tableWrapRef = useRef<HTMLDivElement>(null)

  const groupsQ = useQuery({ queryKey: ['groups'], queryFn: api.groups })
  const tagsQ = useQuery({ queryKey: ['tags'], queryFn: api.tags })
  const agentsQ = useQuery({ queryKey: ['agents'], queryFn: api.agents, refetchInterval: 10000 })

  const agents = useMemo(() => {
    const all = agentsQ.data ?? []
    if (selectedGroupId == null) return all
    return all.filter((a) => a.groupId === selectedGroupId)
  }, [agentsQ.data, selectedGroupId])

  const tableScrollY = useTableScrollY(tableWrapRef, [agentsQ.isLoading, agents.length])

  const patchAgent = useMutation({
    mutationFn: ({ id, body }: { id: string; body: Parameters<typeof api.patchAgent>[1] }) =>
      api.patchAgent(id, body),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['agents'] })
      qc.invalidateQueries({ queryKey: ['tags'] })
      setEditing(null)
      message.success('已保存')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const deleteAgent = useMutation({
    mutationFn: api.deleteAgent,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['agents'] })
      qc.invalidateQueries({ queryKey: ['tags'] })
      message.success('已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const agentColumns = useMemo<ColumnsType<Agent>>(
    () => [
      {
        title: 'Agent ID',
        dataIndex: 'agentId',
        ellipsis: true,
        width: 280,
      },
      {
        title: '显示名',
        dataIndex: 'displayName',
        width: 140,
        ellipsis: true,
        render: (v, r) => v || r.hostname || r.agentId,
      },
      { title: '主机名', dataIndex: 'hostname', width: 140, ellipsis: true },
      {
        title: '状态',
        dataIndex: 'online',
        width: 80,
        render: (online: boolean) => (
          <Tag color={online ? 'green' : 'default'}>{online ? '在线' : '离线'}</Tag>
        ),
      },
      { title: '优选 IPv4', dataIndex: 'preferredIpv4', width: 130 },
      {
        title: '全部 IPv4',
        dataIndex: 'ipv4s',
        width: 200,
        ellipsis: true,
        render: (ips: string[]) => (ips ?? []).join(', '),
      },
      { title: '端口', dataIndex: 'mediaPort', width: 80 },
      { title: '版本', dataIndex: 'version', width: 100, ellipsis: true },
      {
        title: '标签',
        dataIndex: 'tagNames',
        width: 160,
        render: (names: string[]) =>
          (names ?? []).map((n) => (
            <Tag key={n}>{n}</Tag>
          )),
      },
      {
        title: '操作',
        key: 'actions',
        width: 140,
        fixed: 'right',
        render: (_, r) => (
          <Space>
            <Button size="small" onClick={() => setEditing(r)}>
              编辑
            </Button>
            <Button
              size="small"
              danger
              disabled={r.online}
              onClick={() => deleteAgent.mutate(r.agentId)}
            >
              删除
            </Button>
          </Space>
        ),
      },
    ],
    [deleteAgent],
  )

  const resizableAgentColumns = useResizableColumns(agentColumns)

  const createGroup = useMutation({
    mutationFn: (name: string) =>
      api.createGroup({ name, parentId: selectedGroupId ?? undefined }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['groups'] })
      message.success('分组已创建')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const deleteGroup = useMutation({
    mutationFn: api.deleteGroup,
    onSuccess: () => {
      setSelectedGroupId(null)
      qc.invalidateQueries({ queryKey: ['groups'] })
      message.success('分组已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const createTag = useMutation({
    mutationFn: api.createTag,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['tags'] })
      message.success('标签已创建')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const patchTag = useMutation({
    mutationFn: ({ id, name }: { id: number; name: string }) => api.patchTag(id, name),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['tags'] })
      qc.invalidateQueries({ queryKey: ['agents'] })
      message.success('标签已重命名')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const deleteTag = useMutation({
    mutationFn: api.deleteTag,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['tags'] })
      qc.invalidateQueries({ queryKey: ['agents'] })
      message.success('标签已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

  return (
    <Layout className="h-full overflow-hidden">
      <div className="flex shrink-0 items-center justify-end gap-2 border-b border-slate-200 bg-white/70 px-4 py-2">
        <Button
          onClick={async () => {
            try {
              await api.downloadSecret('agent-psk')
            } catch (e) {
              message.error(e instanceof Error ? e.message : '下载失败')
            }
          }}
        >
          下载 Agent PSK
        </Button>
        <Button
          onClick={async () => {
            try {
              await api.downloadSecret('viewer-psk')
            } catch (e) {
              message.error(e instanceof Error ? e.message : '下载失败')
            }
          }}
        >
          下载 Viewer PSK
        </Button>
        <Button onClick={() => setTagModal(true)}>标签管理</Button>
      </div>
      <Layout className="min-h-0 flex-1 overflow-hidden">
        <Sider
          width={280}
          theme="light"
          className="!overflow-auto border-r border-slate-200 p-3"
        >
          <Space className="mb-3" size="small">
            <Tooltip title="新建子分组">
              <Button
                icon={<FolderAddOutlined />}
                onClick={() => {
                  const name = window.prompt('新分组名称')
                  if (name?.trim()) createGroup.mutate(name.trim())
                }}
              />
            </Tooltip>
            <Tooltip title="删除选中分组">
              <Button
                danger
                icon={<DeleteOutlined />}
                disabled={selectedGroupId == null}
                onClick={() => {
                  if (selectedGroupId != null) deleteGroup.mutate(selectedGroupId)
                }}
              />
            </Tooltip>
            <Tooltip title="全部 Agent">
              <Button
                type={selectedGroupId == null ? 'primary' : 'default'}
                icon={<AppstoreOutlined />}
                onClick={() => setSelectedGroupId(null)}
              />
            </Tooltip>
          </Space>
          <Tree
            treeData={buildTree(groupsQ.data ?? [])}
            selectedKeys={selectedGroupId != null ? [String(selectedGroupId)] : []}
            onSelect={(keys) => {
              const k = keys[0]
              setSelectedGroupId(k ? Number(k) : null)
            }}
            defaultExpandAll
          />
        </Sider>
        <Content className="flex min-h-0 flex-1 flex-col overflow-hidden p-4">
          <div ref={tableWrapRef} className="min-h-0 flex-1">
            <Table
              className="admin-table-fill"
              rowKey="agentId"
              loading={agentsQ.isLoading}
              dataSource={agents}
              columns={resizableAgentColumns}
              components={resizableTableComponents}
              scroll={{ x: 1450, y: tableScrollY }}
              tableLayout="fixed"
              pagination={{
                showSizeChanger: true,
                showTotal: (total) => `共 ${total} 条`,
              }}
            />
          </div>
        </Content>
      </Layout>

      <Drawer open={!!editing} onClose={() => setEditing(null)} title="编辑 Agent" width={420}>
        {editing && (
          <Form
            layout="vertical"
            initialValues={{
              displayName: editing.displayName,
              groupId: editing.groupId,
              tagIds: editing.tagIds,
            }}
            onFinish={(v) =>
              patchAgent.mutate({
                id: editing.agentId,
                body: { displayName: v.displayName, groupId: v.groupId, tagIds: v.tagIds },
              })
            }
          >
            <Form.Item name="displayName" label="显示名">
              <Input />
            </Form.Item>
            <Form.Item name="groupId" label="分组" rules={[{ required: true }]}>
              <Select
                options={(groupsQ.data ?? []).map((g) => ({ value: g.id, label: g.name }))}
              />
            </Form.Item>
            <Form.Item name="tagIds" label="标签">
              <Select
                mode="multiple"
                options={(tagsQ.data ?? []).map((t) => ({ value: t.id, label: t.name }))}
              />
            </Form.Item>
            <Button type="primary" htmlType="submit" loading={patchAgent.isPending} block>
              保存
            </Button>
          </Form>
        )}
      </Drawer>

      <Modal open={tagModal} onCancel={() => setTagModal(false)} footer={null} title="标签管理">
        <Space className="mb-3">
          <Button
            onClick={() => {
              const name = window.prompt('标签名称')
              if (name?.trim()) createTag.mutate(name.trim())
            }}
          >
            新建标签
          </Button>
        </Space>
        <Table
          size="small"
          rowKey="id"
          dataSource={tagsQ.data ?? []}
          pagination={false}
          columns={[
            { title: '名称', dataIndex: 'name' },
            {
              title: 'Agent 数',
              dataIndex: 'agentCount',
              width: 100,
              align: 'right',
            },
            {
              title: '操作',
              render: (_, t) => (
                <Space>
                  <Button
                    size="small"
                    onClick={() => {
                      const name = window.prompt('标签名称', t.name)
                      if (name?.trim() && name.trim() !== t.name) {
                        patchTag.mutate({ id: t.id, name: name.trim() })
                      }
                    }}
                  >
                    重命名
                  </Button>
                  <Button size="small" danger onClick={() => deleteTag.mutate(t.id)}>
                    删除
                  </Button>
                </Space>
              ),
            },
          ]}
        />
      </Modal>
    </Layout>
  )
}
