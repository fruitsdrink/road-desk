import { useMemo, useState } from 'react'
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
  Tree,
  Typography,
  message,
} from 'antd'
import type { DataNode } from 'antd/es/tree'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { api, type Agent, type Group } from '@/lib/api'
import { logout } from '@/lib/authStore'
import { useNavigate } from '@tanstack/react-router'

const { Header, Sider, Content } = Layout

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
  const navigate = useNavigate()
  const [selectedGroupId, setSelectedGroupId] = useState<number | null>(null)
  const [editing, setEditing] = useState<Agent | null>(null)
  const [tagModal, setTagModal] = useState(false)

  const groupsQ = useQuery({ queryKey: ['groups'], queryFn: api.groups })
  const tagsQ = useQuery({ queryKey: ['tags'], queryFn: api.tags })
  const agentsQ = useQuery({ queryKey: ['agents'], queryFn: api.agents, refetchInterval: 10000 })

  const agents = useMemo(() => {
    const all = agentsQ.data ?? []
    if (selectedGroupId == null) return all
    return all.filter((a) => a.groupId === selectedGroupId)
  }, [agentsQ.data, selectedGroupId])

  const patchAgent = useMutation({
    mutationFn: ({ id, body }: { id: string; body: Parameters<typeof api.patchAgent>[1] }) =>
      api.patchAgent(id, body),
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['agents'] })
      setEditing(null)
      message.success('已保存')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const deleteAgent = useMutation({
    mutationFn: api.deleteAgent,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['agents'] })
      message.success('已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

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

  const deleteTag = useMutation({
    mutationFn: api.deleteTag,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['tags'] })
      message.success('标签已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

  return (
    <Layout className="min-h-full">
      <Header className="flex items-center justify-between bg-slate-800 px-4">
        <Typography.Title level={4} className="!mb-0 !text-white">
          Road Desk 编目
        </Typography.Title>
        <Space>
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
          <Button
            type="link"
            className="!text-white"
            onClick={() => {
              logout()
              navigate({ to: '/login' })
            }}
          >
            退出
          </Button>
        </Space>
      </Header>
      <Layout>
        <Sider width={280} theme="light" className="border-r border-slate-200 p-3">
          <Space className="mb-3 w-full" direction="vertical">
            <Button
              block
              onClick={() => {
                const name = window.prompt('新分组名称')
                if (name?.trim()) createGroup.mutate(name.trim())
              }}
            >
              新建子分组
            </Button>
            <Button
              block
              danger
              disabled={selectedGroupId == null}
              onClick={() => {
                if (selectedGroupId != null) deleteGroup.mutate(selectedGroupId)
              }}
            >
              删除选中分组
            </Button>
            <Button block type={selectedGroupId == null ? 'primary' : 'default'} onClick={() => setSelectedGroupId(null)}>
              全部 Agent
            </Button>
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
        <Content className="p-4">
          <Table
            rowKey="agentId"
            loading={agentsQ.isLoading}
            dataSource={agents}
            columns={[
              {
                title: '显示名',
                dataIndex: 'displayName',
                render: (v, r) => v || r.hostname || r.agentId,
              },
              { title: '主机名', dataIndex: 'hostname' },
              {
                title: '状态',
                dataIndex: 'online',
                render: (online: boolean) => (
                  <Tag color={online ? 'green' : 'default'}>{online ? '在线' : '离线'}</Tag>
                ),
              },
              { title: '优选 IPv4', dataIndex: 'preferredIpv4' },
              {
                title: '全部 IPv4',
                dataIndex: 'ipv4s',
                render: (ips: string[]) => (ips ?? []).join(', '),
              },
              { title: '端口', dataIndex: 'mediaPort' },
              { title: '版本', dataIndex: 'version' },
              {
                title: '标签',
                dataIndex: 'tagNames',
                render: (names: string[]) =>
                  (names ?? []).map((n) => (
                    <Tag key={n}>{n}</Tag>
                  )),
              },
              {
                title: '操作',
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
            ]}
          />
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
              title: '操作',
              render: (_, t) => (
                <Button size="small" danger onClick={() => deleteTag.mutate(t.id)}>
                  删除
                </Button>
              ),
            },
          ]}
        />
      </Modal>
    </Layout>
  )
}
