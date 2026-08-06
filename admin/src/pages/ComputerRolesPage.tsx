import { useMemo, useRef, useState } from 'react'
import { Button, Form, Input, InputNumber, Modal, Space, Table, message } from 'antd'
import type { ColumnsType } from 'antd/es/table'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { api, type ComputerRole } from '@/lib/api'
import { resizableTableComponents } from '@/components/ResizableTitle'
import { useResizableColumns } from '@/hooks/useResizableColumns'
import { useTableScrollY } from '@/hooks/useTableScrollY'

export function ComputerRolesPage() {
  const qc = useQueryClient()
  const tableWrapRef = useRef<HTMLDivElement>(null)
  const [modalOpen, setModalOpen] = useState(false)
  const [editing, setEditing] = useState<ComputerRole | null>(null)
  const [form] = Form.useForm()

  const rolesQ = useQuery({ queryKey: ['computer-roles'], queryFn: api.computerRoles })
  const tableScrollY = useTableScrollY(tableWrapRef, [rolesQ.isLoading, rolesQ.data?.length])

  const save = useMutation({
    mutationFn: async (v: { name: string; sortOrder: number }) => {
      if (editing) return api.patchComputerRole(editing.id, v)
      return api.createComputerRole(v)
    },
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['computer-roles'] })
      qc.invalidateQueries({ queryKey: ['agents'] })
      setModalOpen(false)
      setEditing(null)
      form.resetFields()
      message.success(editing ? '已保存' : '已创建')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const remove = useMutation({
    mutationFn: api.deleteComputerRole,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['computer-roles'] })
      message.success('已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const columns = useMemo<ColumnsType<ComputerRole>>(
    () => [
      { title: '名称', dataIndex: 'name', ellipsis: true },
      { title: '排序', dataIndex: 'sortOrder', width: 100 },
      {
        title: 'Agent 数',
        dataIndex: 'agentCount',
        width: 110,
        align: 'right',
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
                form.setFieldsValue({ name: r.name, sortOrder: r.sortOrder })
                setModalOpen(true)
              }}
            >
              编辑
            </Button>
            <Button
              size="small"
              danger
              disabled={r.agentCount > 0}
              onClick={() => {
                Modal.confirm({
                  title: `删除设备角色「${r.name}」？`,
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
        <div className="text-base font-medium text-slate-800">设备角色</div>
        <Button
          type="primary"
          onClick={() => {
            setEditing(null)
            form.setFieldsValue({ name: '', sortOrder: 0 })
            setModalOpen(true)
          }}
        >
          新建角色
        </Button>
      </div>
      <div ref={tableWrapRef} className="min-h-0 flex-1">
        <Table
          className="admin-table-fill"
          rowKey="id"
          loading={rolesQ.isLoading}
          dataSource={rolesQ.data ?? []}
          columns={resizableColumns}
          components={resizableTableComponents}
          scroll={{ x: 600, y: tableScrollY }}
          tableLayout="fixed"
          pagination={{
            showSizeChanger: true,
            showTotal: (total) => `共 ${total} 条`,
          }}
        />
      </div>

      <Modal
        open={modalOpen}
        title={editing ? '编辑设备角色' : '新建设备角色'}
        onCancel={() => {
          setModalOpen(false)
          setEditing(null)
        }}
        onOk={() => form.submit()}
        confirmLoading={save.isPending}
        destroyOnClose
      >
        <Form form={form} layout="vertical" onFinish={(v) => save.mutate(v)}>
          <Form.Item name="name" label="名称" rules={[{ required: true, message: '请输入名称' }]}>
            <Input maxLength={64} placeholder="如：收费" />
          </Form.Item>
          <Form.Item name="sortOrder" label="排序" initialValue={0}>
            <InputNumber className="w-full" />
          </Form.Item>
        </Form>
      </Modal>
    </div>
  )
}
