import { Button, Card, Form, Input, Typography, message } from 'antd'
import { useNavigate } from '@tanstack/react-router'
import { api } from '@/lib/api'
import { loginWithToken } from '@/lib/authStore'

const devPassword = import.meta.env.DEV ? (import.meta.env.VITE_DEV_ADMIN_PASSWORD ?? '') : ''

export function LoginPage() {
  const navigate = useNavigate()
  const [form] = Form.useForm()

  return (
    <div className="min-h-full flex items-center justify-center p-6">
      <Card className="w-full max-w-md shadow-md" title="Road Desk 管理端">
        <Typography.Paragraph type="secondary">
          仅管理员可登录管理端；操作员请使用 Viewer。
        </Typography.Paragraph>
        <Form
          form={form}
          layout="vertical"
          onFinish={async (v) => {
            try {
              const { token } = await api.login(v.username, v.password)
              loginWithToken(token)
              message.success('登录成功')
              navigate({ to: '/' })
            } catch (e) {
              message.error(e instanceof Error ? e.message : '登录失败')
            }
          }}
        >
          <Form.Item name="username" label="用户名" rules={[{ required: true }]} initialValue="admin">
            <Input autoFocus={!devPassword} />
          </Form.Item>
          <Form.Item
            name="password"
            label="口令"
            rules={[{ required: true }]}
            initialValue={devPassword}
          >
            <Input.Password autoFocus={!!devPassword} />
          </Form.Item>
          <Button type="primary" htmlType="submit" block>
            登录
          </Button>
        </Form>
      </Card>
    </div>
  )
}
