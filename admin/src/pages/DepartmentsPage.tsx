import { useMemo, useRef, useState } from 'react'
import { Button, Form, Input, InputNumber, Modal, Space, Table, Tag, message } from 'antd'
import type { ColumnsType } from 'antd/es/table'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { api, type Department } from '@/lib/api'
import { resizableTableComponents } from '@/components/ResizableTitle'
import { useResizableColumns } from '@/hooks/useResizableColumns'
import { useTableScrollY } from '@/hooks/useTableScrollY'

export function DepartmentsPage() {
  const qc = useQueryClient()
  const tableWrapRef = useRef<HTMLDivElement>(null)
  const [modalOpen, setModalOpen] = useState(false)
  const [editing, setEditing] = useState<Department | null>(null)
  const [form] = Form.useForm()

  const deptsQ = useQuery({ queryKey: ['departments'], queryFn: api.departments })
  const tableScrollY = useTableScrollY(tableWrapRef, [deptsQ.isLoading, deptsQ.data?.length])

  const save = useMutation({
    mutationFn: async (v: { name: string; sortOrder: number }) => {
      if (editing) return api.patchDepartment(editing.id, v)
      return api.createDepartment(v)
    },
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['departments'] })
      setModalOpen(false)
      setEditing(null)
      form.resetFields()
      message.success(editing ? '已保存' : '已创建')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const remove = useMutation({
    mutationFn: api.deleteDepartment,
    onSuccess: () => {
      qc.invalidateQueries({ queryKey: ['departments'] })
      message.success('已删除')
    },
    onError: (e: Error) => message.error(e.message),
  })

  const columns = useMemo<ColumnsType<Department>>(
    () => [
      { title: '名称', dataIndex: 'name', ellipsis: true },
      { title: '排序', dataIndex: 'sortOrder', width: 100 },
      {
        title: '用户数',
        dataIndex: 'userCount',
        width: 100,
        align: 'right',
      },
      {
        title: '类型',
        dataIndex: 'isSystem',
        width: 100,
        render: (v: boolean) => (v ? <Tag>系统</Tag> : <Tag color="blue">自定义</Tag>),
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
              disabled={r.isSystem || r.userCount > 0}
              onClick={() => {
                Modal.confirm({
                  title: `删除部门「${r.name}」？`,
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
        <div className="text-base font-medium text-slate-800">部门管理</div>
        <Button
          type="primary"
          onClick={() => {
            setEditing(null)
            form.setFieldsValue({ name: '', sortOrder: 0 })
            setModalOpen(true)
          }}
        >
          新建部门
        </Button>
      </div>
      <div ref={tableWrapRef} className="min-h-0 flex-1">
        <Table
          className="admin-table-fill"
          rowKey="id"
          loading={deptsQ.isLoading}
          dataSource={deptsQ.data ?? []}
          columns={resizableColumns}
          components={resizableTableComponents}
          scroll={{ x: 700, y: tableScrollY }}
          tableLayout="fixed"
          pagination={{
            showSizeChanger: true,
            showTotal: (total) => `共 ${total} 条`,
          }}
        />
      </div>

      <Modal
        open={modalOpen}
        title={editing ? '编辑部门' : '新建部门'}
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
            <Input />
          </Form.Item>
          <Form.Item name="sortOrder" label="排序" initialValue={0}>
            <InputNumber className="w-full" />
          </Form.Item>
        </Form>
      </Modal>
    </div>
  )
}
