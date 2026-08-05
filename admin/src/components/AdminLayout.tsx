import { Button, Layout, Menu, Space, Typography } from 'antd'
import { Outlet, useNavigate, useRouterState } from '@tanstack/react-router'
import { logout } from '@/lib/authStore'

const { Header, Content } = Layout

const navItems = [
  { key: '/', label: '编目' },
  { key: '/departments', label: '部门' },
  { key: '/users', label: '用户' },
  { key: '/audit', label: '审计' },
]

export function AdminLayout() {
  const navigate = useNavigate()
  const pathname = useRouterState({ select: (s) => s.location.pathname })
  const selected = navItems.find((i) => i.key === pathname)?.key ?? '/'

  return (
    <Layout className="h-screen overflow-hidden">
      <Header className="flex shrink-0 items-center gap-4 bg-slate-800 px-4">
        <Typography.Title level={4} className="!mb-0 !shrink-0 !text-white">
          Road Desk
        </Typography.Title>
        <Menu
          theme="dark"
          mode="horizontal"
          selectedKeys={[selected]}
          items={navItems}
          className="min-w-0 flex-1 border-none bg-transparent"
          onClick={({ key }) => navigate({ to: key })}
        />
        <Space>
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
      <Content className="min-h-0 flex-1 overflow-hidden">
        <Outlet />
      </Content>
    </Layout>
  )
}
