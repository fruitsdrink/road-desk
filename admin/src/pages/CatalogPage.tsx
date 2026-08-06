import { useMemo, useRef, useState, type ReactNode } from 'react'
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
  TreeSelect,
  message,
} from 'antd'
import {
  AppstoreOutlined,
  DeleteOutlined,
  EditOutlined,
  FolderAddOutlined,
  PlusOutlined,
} from '@ant-design/icons'
import type { ColumnsType } from 'antd/es/table'
import type { DataNode, TreeProps } from 'antd/es/tree'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { api, type Agent, type Group } from '@/lib/api'
import { resizableTableComponents } from '@/components/ResizableTitle'
import { useResizableColumns } from '@/hooks/useResizableColumns'
import { useTableScrollY } from '@/hooks/useTableScrollY'

const { Sider, Content } = Layout

type GroupTreeNode = {
  value: number
  title: string
  children?: GroupTreeNode[]
}

function groupsByParent(groups: Group[]): Map<number | null, Group[]> {
  const byParent = new Map<number | null, Group[]>()
  for (const g of groups) {
    const key = g.parentId
    const list = byParent.get(key) ?? []
    list.push(g)
    byParent.set(key, list)
  }
  for (const list of byParent.values()) {
    list.sort((a, b) => a.sortOrder - b.sortOrder || a.id - b.id)
  }
  return byParent
}

function buildTree(groups: Group[]): DataNode[] {
  const byParent = groupsByParent(groups)
  const walk = (parentId: number | null): DataNode[] =>
    (byParent.get(parentId) ?? []).map((g) => ({
      key: String(g.id),
      title: g.name,
      children: walk(g.id),
    }))
  return walk(null)
}

function buildGroupTreeSelect(groups: Group[], excludeIds?: Set<number>): GroupTreeNode[] {
  const byParent = groupsByParent(groups)
  const walk = (parentId: number | null): GroupTreeNode[] =>
    (byParent.get(parentId) ?? [])
      .filter((g) => !excludeIds?.has(g.id))
      .map((g) => ({
        value: g.id,
        title: g.name,
        children: walk(g.id),
      }))
  return walk(null)
}

/** Selected group + all descendant group ids. */
function groupIdWithDescendants(groups: Group[], rootId: number): Set<number> {
  const byParent = groupsByParent(groups)
  const out = new Set<number>()
  const walk = (id: number) => {
    out.add(id)
    for (const c of byParent.get(id) ?? []) {
      walk(c.id)
    }
  }
  walk(rootId)
  return out
}

export function CatalogPage() {
  const qc = useQueryClient()
  const [selectedGroupId, setSelectedGroupId] = useState<number | null>(null)
  const [editing, setEditing] = useState<Agent | null>(null)
  const [tagModal, setTagModal] = useState(false)
  const [groupModal, setGroupModal] = useState<null | 'create-root' | 'create-child' | 'edit'>(null)
  const [groupForm] = Form.useForm<{ name: string; parentId?: number | null }>()
  const tableWrapRef = useRef<HTMLDivElement>(null)

  const groupsQ = useQuery({ queryKey: ['groups'], queryFn: api.groups })
  const tagsQ = useQuery({ queryKey: ['tags'], queryFn: api.tags })
  const rolesQ = useQuery({ queryKey: ['computer-roles'], queryFn: api.computerRoles })
  const agentsQ = useQuery({ queryKey: ['agents'], queryFn: api.agents, refetchInterval: 10000 })

  const groups = groupsQ.data ?? []
  const selectedGroup = useMemo(
    () => groups.find((g) => g.id === selectedGroupId) ?? null,
    [groups, selectedGroupId],
  )

  const groupTreeSelectData = useMemo(() => buildGroupTreeSelect(groups), [groups])

  const editParentTreeData = useMemo(() => {
    if (selectedGroupId == null) return groupTreeSelectData
    return buildGroupTreeSelect(groups, groupIdWithDescendants(groups, selectedGroupId))
  }, [groups, groupTreeSelectData, selectedGroupId])

  const agents = useMemo(() => {
    const all = agentsQ.data ?? []
    if (selectedGroupId == null) return all
    const ids = groupIdWithDescendants(groups, selectedGroupId)
    return all.filter((a) => ids.has(a.groupId))
  }, [agentsQ.data, groups, selectedGroupId])

  const tableScrollY = useTableScrollY(tableWrapRef, [agentsQ.isLoading, agents.length])

  const patchAgent = useMutation({
    mutationFn: ({ id, body }: { id: string; body: Parameters<typeof api.patchAgent>[1] }) =>
      api.patchAgent(id, body),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['agents'] })
      qc.invalidateQueries({ queryKey: ['tags'] })
      qc.invalidateQueries({ queryKey: ['computer-roles'] })
      setEditing(null)
      message.success('已保存')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const moveAgentToGroup = useMutation({
    mutationFn: ({ id, groupId }: { id: string; groupId: number }) =>
      api.patchAgent(id, { groupId }),
    onSuccess: (_, vars) => {
      qc.invalidateQueries({ queryKey: ['agents'] })
      const g = groups.find((x) => x.id === vars.groupId)
      message.success(`已移动到「${g?.name ?? vars.groupId}」`)
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
        title: '设备角色',
        dataIndex: 'computerRole',
        width: 120,
        render: (v: string) => (v ? <Tag color="blue">{v}</Tag> : <span className="text-gray-400">—</span>),
      },
      {
        title: '安装位置',
        dataIndex: 'installLocation',
        width: 160,
        ellipsis: true,
        render: (v: string) => v || <span className="text-gray-400">—</span>,
      },
      {
        title: '车道编号',
        dataIndex: 'laneNumber',
        width: 100,
        ellipsis: true,
        render: (v: string) => v || <span className="text-gray-400">—</span>,
      },
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
    mutationFn: (body: { name: string; parentId?: number }) => api.createGroup(body),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['groups'] })
      setGroupModal(null)
      groupForm.resetFields()
      message.success('分组已创建')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const patchGroup = useMutation({
    mutationFn: ({
      id,
      name,
      parentId,
    }: {
      id: number
      name: string
      parentId: number | null
    }) => api.patchGroup(id, { name, parentId }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['groups'] })
      setGroupModal(null)
      groupForm.resetFields()
      message.success('分组已更新')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const moveGroup = useMutation({
    mutationFn: ({
      id,
      parentId,
      index,
    }: {
      id: number
      parentId: number | null
      index: number
    }) => api.moveGroup(id, { parentId, index }),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['groups'] })
    },
    onError: (e: Error) => message.error(e.message),
  })

  const deleteGroup = useMutation({
    mutationFn: ({ id, cascade }: { id: number; cascade?: boolean }) =>
      api.deleteGroup(id, { cascade }),
    onSuccess: () => {
      setSelectedGroupId(null)
      qc.invalidateQueries({ queryKey: ['groups'] })
      qc.invalidateQueries({ queryKey: ['agents'] })
      message.success('分组已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const openGroupModal = (mode: 'create-root' | 'create-child' | 'edit') => {
    if (mode === 'create-child' && selectedGroupId == null) {
      message.warning('请先选中父分组')
      return
    }
    if (mode === 'edit') {
      if (!selectedGroup) {
        message.warning('请先选中要编辑的分组')
        return
      }
      groupForm.setFieldsValue({
        name: selectedGroup.name,
        parentId: selectedGroup.parentId ?? null,
      })
    } else {
      groupForm.resetFields()
    }
    setGroupModal(mode)
  }

  const submitGroupModal = (v: { name: string; parentId?: number | null }) => {
    const name = v.name.trim()
    if (!name) return
    if (groupModal === 'create-root') {
      createGroup.mutate({ name })
      return
    }
    if (groupModal === 'create-child' && selectedGroupId != null) {
      createGroup.mutate({ name, parentId: selectedGroupId })
      return
    }
    if (groupModal === 'edit' && selectedGroupId != null) {
      patchGroup.mutate({
        id: selectedGroupId,
        name,
        parentId: v.parentId ?? null,
      })
    }
  }

  const onGroupDrop: TreeProps['onDrop'] = (info) => {
    const dragId = Number(info.dragNode.key)
    const dropId = Number(info.node.key)
    if (!Number.isFinite(dragId) || !Number.isFinite(dropId) || dragId === dropId) return

    const dragDescendants = groupIdWithDescendants(groups, dragId)
    const dropGroup = groups.find((g) => g.id === dropId)
    if (!dropGroup) return

    let newParentId: number | null
    let index: number

    if (!info.dropToGap) {
      // Drop onto node → become its child (append).
      if (dragDescendants.has(dropId)) {
        message.warning('不能移动到自己的子分组下')
        return
      }
      newParentId = dropId
      const kids = groups
        .filter((g) => g.parentId === dropId && g.id !== dragId)
        .sort((a, b) => a.sortOrder - b.sortOrder || a.id - b.id)
      index = kids.length
    } else {
      const dropPos = info.node.pos.split('-')
      const dropPosition = info.dropPosition - Number(dropPos[dropPos.length - 1])
      newParentId = dropGroup.parentId ?? null
      if (newParentId != null && dragDescendants.has(newParentId)) {
        message.warning('不能移动到自己的子分组下')
        return
      }
      const siblings = groups
        .filter((g) => (g.parentId ?? null) === newParentId && g.id !== dragId)
        .sort((a, b) => a.sortOrder - b.sortOrder || a.id - b.id)
      const dropIndex = siblings.findIndex((g) => g.id === dropId)
      const at = dropIndex < 0 ? siblings.length : dropIndex
      index = dropPosition === -1 ? at : at + 1
    }

    moveGroup.mutate({ id: dragId, parentId: newParentId, index })
  }

  const allowGroupDrop: TreeProps['allowDrop'] = ({ dragNode, dropNode, dropPosition }) => {
    const dragId = Number(dragNode.key)
    const dropId = Number(dropNode.key)
    if (!Number.isFinite(dragId) || !Number.isFinite(dropId) || dragId === dropId) return false
    const descendants = groupIdWithDescendants(groups, dragId)
    if (dropPosition === 0) {
      // Drop as child of dropNode
      return !descendants.has(dropId)
    }
    const dropGroup = groups.find((g) => g.id === dropId)
    const parentId = dropGroup?.parentId ?? null
    if (parentId != null && descendants.has(parentId)) return false
    return true
  }

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
          width={300}
          theme="light"
          className="!overflow-auto border-r border-slate-200 p-3"
        >
          <div className="mb-2 flex items-center gap-1">
            <Tooltip title="在根级新建分组">
              <Button size="small" icon={<PlusOutlined />} onClick={() => openGroupModal('create-root')}>
                根分组
              </Button>
            </Tooltip>
            <Tooltip title={selectedGroup ? `在「${selectedGroup.name}」下增加子分组` : '请先选中父分组'}>
              <Button
                size="small"
                icon={<FolderAddOutlined />}
                disabled={selectedGroupId == null}
                onClick={() => openGroupModal('create-child')}
              />
            </Tooltip>
            <Tooltip title={selectedGroup ? `编辑「${selectedGroup.name}」` : '请先选中分组'}>
              <Button
                size="small"
                icon={<EditOutlined />}
                disabled={selectedGroupId == null}
                onClick={() => openGroupModal('edit')}
              />
            </Tooltip>
            <Tooltip title="删除选中分组">
              <Button
                size="small"
                danger
                icon={<DeleteOutlined />}
                disabled={selectedGroupId == null || !!selectedGroup?.isSystem}
                onClick={() => {
                  if (selectedGroupId == null || !selectedGroup) return
                  const subtreeIds = groupIdWithDescendants(groups, selectedGroupId)
                  const childGroupCount = [...subtreeIds].filter((id) => id !== selectedGroupId).length
                  const agentCount = (agentsQ.data ?? []).filter((a) => subtreeIds.has(a.groupId)).length
                  const hasContent = childGroupCount > 0 || agentCount > 0
                  const parts: string[] = []
                  if (childGroupCount > 0) parts.push(`${childGroupCount} 个子分组`)
                  if (agentCount > 0) parts.push(`${agentCount} 台 Agent（将移至「未分类」）`)
                  Modal.confirm({
                    title: `删除分组「${selectedGroup.name}」？`,
                    content: hasContent
                      ? `该分组下还有 ${parts.join('、')}。确认后将一并删除所有子分组，Agent 移到「未分类」。此操作不可撤销。`
                      : '确认删除该空分组？',
                    okText: hasContent ? '确认删除' : '删除',
                    okType: 'danger',
                    cancelText: '取消',
                    onOk: () =>
                      deleteGroup.mutateAsync({ id: selectedGroupId, cascade: hasContent }),
                  })
                }}
              />
            </Tooltip>
            <Tooltip title="显示全部 Agent（不按分组筛选）">
              <Button
                size="small"
                className="ml-auto"
                type={selectedGroupId == null ? 'primary' : 'default'}
                icon={<AppstoreOutlined />}
                onClick={() => setSelectedGroupId(null)}
              >
                全部
              </Button>
            </Tooltip>
          </div>
          {selectedGroup ? (
            <div className="mb-2 truncate text-xs text-slate-500" title={selectedGroup.name}>
              当前：{selectedGroup.name}
              {selectedGroup.isSystem ? '（系统）' : ''}
            </div>
          ) : (
            <div className="mb-2 text-xs text-slate-400">拖分组调层级；拖 Agent 到分组归类</div>
          )}
          <Tree
            treeData={buildTree(groups)}
            selectedKeys={selectedGroupId != null ? [String(selectedGroupId)] : []}
            onSelect={(keys) => {
              const k = keys[0]
              setSelectedGroupId(k ? Number(k) : null)
            }}
            defaultExpandAll
            draggable
            blockNode
            allowDrop={allowGroupDrop}
            onDrop={onGroupDrop}
            titleRender={(node) => {
              const groupId = Number(node.key)
              return (
                <span
                  className="inline-block w-full"
                  onDragOver={(e) => {
                    const types = Array.from(e.dataTransfer.types)
                    if (
                      !types.includes('application/x-road-desk-agent') &&
                      !types.includes('text/plain')
                    ) {
                      return
                    }
                    e.preventDefault()
                    e.stopPropagation()
                    e.dataTransfer.dropEffect = 'move'
                  }}
                  onDrop={(e) => {
                    let agentId = e.dataTransfer.getData('application/x-road-desk-agent')
                    if (!agentId) {
                      const plain = e.dataTransfer.getData('text/plain')
                      if (plain.startsWith('agent:')) agentId = plain.slice(6)
                    }
                    if (!agentId || !Number.isFinite(groupId)) return
                    e.preventDefault()
                    e.stopPropagation()
                    const agent = (agentsQ.data ?? []).find((a) => a.agentId === agentId)
                    if (agent && agent.groupId === groupId) {
                      message.info('已在该分组中')
                      return
                    }
                    moveAgentToGroup.mutate({ id: agentId, groupId })
                  }}
                >
                  {node.title as ReactNode}
                </span>
              )
            }}
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
              scroll={{ x: 1840, y: tableScrollY }}
              tableLayout="fixed"
              onRow={(record) => ({
                draggable: true,
                className: 'cursor-grab active:cursor-grabbing',
                title: '拖到左侧分组可更改归属',
                onDragStart: (e) => {
                  e.dataTransfer.setData('application/x-road-desk-agent', record.agentId)
                  e.dataTransfer.setData('text/plain', `agent:${record.agentId}`)
                  e.dataTransfer.effectAllowed = 'move'
                },
              })}
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
              computerRole: editing.computerRole || undefined,
              installLocation: editing.installLocation || undefined,
              laneNumber: editing.laneNumber || undefined,
              tagIds: editing.tagIds,
            }}
            onFinish={(v) =>
              patchAgent.mutate({
                id: editing.agentId,
                body: {
                  displayName: v.displayName,
                  groupId: v.groupId,
                  computerRole: v.computerRole ?? '',
                  installLocation: (v.installLocation ?? '').trim(),
                  laneNumber: (v.laneNumber ?? '').trim(),
                  tagIds: v.tagIds,
                },
              })
            }
          >
            <Form.Item name="displayName" label="显示名">
              <Input />
            </Form.Item>
            <Form.Item name="groupId" label="分组" rules={[{ required: true }]}>
              <TreeSelect
                treeData={groupTreeSelectData}
                treeDefaultExpandAll
                showSearch
                treeNodeFilterProp="title"
                placeholder="选择分组"
                style={{ width: '100%' }}
              />
            </Form.Item>
            <Form.Item name="computerRole" label="设备角色">
              <Select
                allowClear
                placeholder="未设置"
                options={(rolesQ.data ?? []).map((r) => ({ value: r.name, label: r.name }))}
              />
            </Form.Item>
            <Form.Item name="installLocation" label="安装位置">
              <Input placeholder="如：入口01车道" maxLength={120} allowClear />
            </Form.Item>
            <Form.Item name="laneNumber" label="车道编号">
              <Input placeholder="可空，如：01" maxLength={32} allowClear />
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

      <Modal
        open={groupModal != null}
        title={
          groupModal === 'create-root'
            ? '新建根分组'
            : groupModal === 'create-child'
              ? `增加子分组（父级：${selectedGroup?.name ?? ''}）`
              : `编辑分组${selectedGroup ? `「${selectedGroup.name}」` : ''}`
        }
        onCancel={() => {
          setGroupModal(null)
          groupForm.resetFields()
        }}
        onOk={() => groupForm.submit()}
        confirmLoading={createGroup.isPending || patchGroup.isPending}
        destroyOnClose
      >
        <Form form={groupForm} layout="vertical" onFinish={submitGroupModal}>
          <Form.Item
            name="name"
            label="分组名称"
            rules={[{ required: true, message: '请输入分组名称' }]}
          >
            <Input placeholder="如：入口 / 出口 / 一号岗亭" maxLength={64} autoFocus />
          </Form.Item>
          {groupModal === 'edit' && (
            <Form.Item
              name="parentId"
              label="父分组"
              extra="清空则为根分组；不可选自身或子分组"
            >
              <TreeSelect
                allowClear
                placeholder="无（根分组）"
                treeData={editParentTreeData}
                treeDefaultExpandAll
                showSearch
                treeNodeFilterProp="title"
                style={{ width: '100%' }}
              />
            </Form.Item>
          )}
        </Form>
      </Modal>

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
