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
import { authStore } from './lib/authStore'
import { LoginPage } from './pages/LoginPage'
import { CatalogPage } from './pages/CatalogPage'
import './styles.css'

const queryClient = new QueryClient()

function AuthGate() {
  useStore(authStore)
  return <Outlet />
}

const rootRoute = createRootRoute({
  component: AuthGate,
})

const loginRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/login',
  component: LoginPage,
})

const indexRoute = createRoute({
  getParentRoute: () => rootRoute,
  path: '/',
  beforeLoad: () => {
    if (!authStore.state.token) {
      throw redirect({ to: '/login' })
    }
  },
  component: CatalogPage,
})

const router = createRouter({
  routeTree: rootRoute.addChildren([loginRoute, indexRoute]),
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
