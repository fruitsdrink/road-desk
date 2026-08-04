import { useMemo, useRef, useState } from 'react'
import {
  Button,
  Form,
  Input,
  Modal,
  Select,
  Space,
  Switch,
  Table,
  Tag,
  message,
} from 'antd'
import type { ColumnsType } from 'antd/es/table'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { api, type User, type UserRole } from '@/lib/api'
import { resizableTableComponents } from '@/components/ResizableTitle'
import { useResizableColumns } from '@/hooks/useResizableColumns'
import { useTableScrollY } from '@/hooks/useTableScrollY'

const roleLabel: Record<UserRole, string> = {
  admin: '管理员',
  viewer: '操作员',
}

export function UsersPage() {
  const qc = useQueryClient()
  const tableWrapRef = useRef<HTMLDivElement>(null)
  const [modalOpen, setModalOpen] = useState(false)
  const [editing, setEditing] = useState<User | null>(null)
  const [form] = Form.useForm()

  const usersQ = useQuery({ queryKey: ['users'], queryFn: api.users })
  const deptsQ = useQuery({ queryKey: ['departments'], queryFn: api.departments })
  const tableScrollY = useTableScrollY(tableWrapRef, [usersQ.isLoading, usersQ.data?.length])

  const save = useMutation({
    mutationFn: async (v: {
      username: string
      password?: string
      role: UserRole
      departmentId: number
      enabled: boolean
    }) => {
      if (editing) {
        const body: Parameters<typeof api.patchUser>[1] = {
          role: v.role,
          departmentId: v.departmentId,
          enabled: v.enabled,
        }
        if (v.password) body.password = v.password
        return api.patchUser(editing.id, body)
      }
      if (!v.password) throw new Error('password required')
      return api.createUser({
        username: v.username,
        password: v.password,
        role: v.role,
        departmentId: v.departmentId,
        enabled: v.enabled,
      })
    },
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['users'] })
      qc.invalidateQueries({ queryKey: ['departments'] })
      setModalOpen(false)
      setEditing(null)
      form.resetFields()
      message.success(editing ? '已保存' : '已创建')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const remove = useMutation({
    mutationFn: api.deleteUser,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['users'] })
      qc.invalidateQueries({ queryKey: ['departments'] })
      message.success('已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const columns = useMemo<ColumnsType<User>>(
    () => [
      { title: '用户名', dataIndex: 'username', width: 160, ellipsis: true },
      {
        title: '角色',
        dataIndex: 'role',
        width: 110,
        render: (role: UserRole) => (
          <Tag color={role === 'admin' ? 'gold' : 'blue'}>{roleLabel[role]}</Tag>
        ),
      },
      { title: '部门', dataIndex: 'departmentName', ellipsis: true },
      {
        title: '状态',
        dataIndex: 'enabled',
        width: 90,
        render: (enabled: boolean) =>
          enabled ? <Tag color="green">启用</Tag> : <Tag>停用</Tag>,
      },
      {
        title: '操作',
        key: 'actions',
        width: 160,
        fixed: 'right',
        render: (_, r) => (
          <Space>
            <Button
              size="small"
              onClick={() => {
                setEditing(r)
                form.setFieldsValue({
                  username: r.username,
                  password: '',
                  role: r.role,
                  departmentId: r.departmentId,
                  enabled: r.enabled,
                })
                setModalOpen(true)
              }}
            >
              编辑
            </Button>
            <Button
              size="small"
              danger
              onClick={() => {
                Modal.confirm({
                  title: `删除用户「${r.username}」？`,
                  onOk: () => remove.mutateAsync(r.id),
                })
              }}
            >
              删除
            </Button>
          </Space>
        ),
      },
    ],
    [form, remove],
  )

  const resizableColumns = useResizableColumns(columns)

  return (
    <div className="flex h-full min-h-0 flex-col gap-3 p-4">
      <div className="flex shrink-0 items-center justify-between">
        <div className="text-base font-medium text-slate-800">用户管理</div>
        <Button
          type="primary"
          onClick={() => {
            setEditing(null)
            form.setFieldsValue({
              username: '',
              password: '',
              role: 'viewer',
              departmentId: deptsQ.data?.[0]?.id,
              enabled: true,
            })
            setModalOpen(true)
          }}
        >
          新建用户
        </Button>
      </div>
      <div ref={tableWrapRef} className="min-h-0 flex-1">
        <Table
          className="admin-table-fill"
          rowKey="id"
          loading={usersQ.isLoading}
          dataSource={usersQ.data ?? []}
          columns={resizableColumns}
          components={resizableTableComponents}
          scroll={{ x: 800, y: tableScrollY }}
          tableLayout="fixed"
          pagination={{
            showSizeChanger: true,
            showTotal: (total) => `共 ${total} 条`,
          }}
        />
      </div>

      <Modal
        open={modalOpen}
        title={editing ? '编辑用户' : '新建用户'}
        onCancel={() => {
          setModalOpen(false)
          setEditing(null)
        }}
        onOk={() => form.submit()}
        confirmLoading={save.isPending}
        destroyOnClose
      >
        <Form form={form} layout="vertical" onFinish={(v) => save.mutate(v)}>
          <Form.Item
            name="username"
            label="用户名"
            rules={[{ required: true, message: '请输入用户名' }]}
          >
            <Input disabled={!!editing} autoComplete="off" />
          </Form.Item>
          <Form.Item
            name="password"
            label={editing ? '新口令（留空不改）' : '口令'}
            rules={editing ? [] : [{ required: true, message: '请输入口令' }]}
          >
            <Input.Password autoComplete="new-password" />
          </Form.Item>
          <Form.Item name="role" label="角色" rules={[{ required: true }]}>
            <Select
              options={[
                { value: 'admin', label: '管理员（可登录管理端）' },
                { value: 'viewer', label: '操作员（仅 Viewer）' },
              ]}
            />
          </Form.Item>
          <Form.Item name="departmentId" label="部门" rules={[{ required: true, message: '请选择部门' }]}>
            <Select
              options={(deptsQ.data ?? []).map((d) => ({ value: d.id, label: d.name }))}
            />
          </Form.Item>
          <Form.Item name="enabled" label="启用" valuePropName="checked">
            <Switch />
          </Form.Item>
        </Form>
      </Modal>
    </div>
  )
}
