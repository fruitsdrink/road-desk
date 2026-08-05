import React from 'react'
import ReactDOM from 'react-dom/client'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import {
  Outlet,
  RouterProvider,
  createRootRoute,
  createRoute,
  createRouter,
  redirect,
} from '@tanstack/react-router'
import { useStore } from '@tanstack/react-store'
import { ConfigProvider } from 'antd'
import zhCN from 'antd/locale/zh_CN'
import { AdminLayout } from './components/AdminLayout'
import { authStore } from './lib/authStore'
import { LoginPage } from './pages/LoginPage'
import { CatalogPage } from './pages/CatalogPage'
import { DepartmentsPage } from './pages/DepartmentsPage'
import { UsersPage } from './pages/UsersPage'
import { AuditPage } from './pages/AuditPage'
import './styles.css'

const queryClient = new QueryClient()

function AuthGate() {
  useStore(authStore)
  return (
    <div className="h-full">
      <Outlet />
    </div>
  )
}

const rootRoute = createRootRoute({
  component: AuthGate,
})

const loginRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/login',
  component: LoginPage,
})

const appRoute = createRoute({
  getParentRoute: () => rootRoute,
  id: 'app',
  beforeLoad: () => {
    if (!authStore.state.token) {
      throw redirect({ to: '/login' })
    }
  },
  component: AdminLayout,
})

const indexRoute = createRoute({
  getParentRoute: () => appRoute,
  path: '/',
  component: CatalogPage,
})

const departmentsRoute = createRoute({
  getParentRoute: () => appRoute,
  path: '/departments',
  component: DepartmentsPage,
})

const usersRoute = createRoute({
  getParentRoute: () => appRoute,
  path: '/users',
  component: UsersPage,
})

const auditRoute = createRoute({
  getParentRoute: () => appRoute,
  path: '/audit',
  component: AuditPage,
})

const router = createRouter({
  routeTree: rootRoute.addChildren([
    loginRoute,
    appRoute.addChildren([indexRoute, departmentsRoute, usersRoute, auditRoute]),
  ]),
})

declare module '@tanstack/react-router' {
  interface Register {
    router: typeof router
  }
}

ReactDOM.createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <ConfigProvider locale={zhCN}>
      <QueryClientProvider client={queryClient}>
        <RouterProvider router={router} />
      </QueryClientProvider>
    </ConfigProvider>
  </React.StrictMode>,
)
