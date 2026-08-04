import { getToken, logoutToLogin } from './authStore'

export class ApiError extends Error {
  status: number
  constructor(status: number, message: string) {
    super(message)
    this.status = status
  }
}

function handleUnauthorized(status: number) {
  if (status === 401) logoutToLogin()
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const headers = new Headers(init.headers)
  if (!headers.has('Content-Type') && init.body) {
    headers.set('Content-Type', 'application/json')
  }
  const token = getToken()
  if (token) headers.set('Authorization', `Bearer ${token}`)
  const res = await fetch(path, { ...init, headers })
  if (res.status === 204) return undefined as T
  const text = await res.text()
  const data = text ? JSON.parse(text) : null
  if (res.status === 401) {
    // Login itself returns 401 for bad password — don't treat as expired session.
    if (path !== '/v1/admin/login' && path !== '/v1/viewer/login') handleUnauthorized(401)
    throw new ApiError(401, data?.error || '未授权')
  }
  if (!res.ok) {
    throw new ApiError(res.status, data?.error || res.statusText || '请求失败')
  }
  return data as T
}

export type Group = {
  id: number
  parentId: number | null
  name: string
  sortOrder: number
  isSystem: boolean
}

export type Tag = { id: number; name: string; agentCount: number }

export type Agent = {
  agentId: string
  displayName: string
  hostname: string
  groupId: number
  mediaPort: number
  version: string
  preferredIpv4: string
  ipv4s: string[]
  tagIds: number[]
  tagNames?: string[]
  lastSeenAt: string | null
  online: boolean
}

export type Department = {
  id: number
  name: string
  sortOrder: number
  isSystem: boolean
  userCount: number
  createdAt: string
}

export type UserRole = 'admin' | 'viewer'

export type User = {
  id: number
  username: string
  role: UserRole
  departmentId: number
  departmentName: string
  enabled: boolean
  createdAt: string
  updatedAt: string
}

export type LoginResult = {
  token: string
  role: UserRole
  username: string
  departmentId: number
  userId: number
}

export const api = {
  login: (username: string, password: string) =>
    request<LoginResult>('/v1/admin/login', {
      method: 'POST',
      body: JSON.stringify({ username, password }),
    }),
  groups: () => request<Group[]>('/v1/admin/groups'),
  createGroup: (body: { name: string; parentId?: number | null; sortOrder?: number }) =>
    request<Group>('/v1/admin/groups', { method: 'POST', body: JSON.stringify(body) }),
  patchGroup: (id: number, body: Partial<{ name: string; parentId: number | null; sortOrder: number }>) =>
    request<Group>(`/v1/admin/groups/${id}`, { method: 'PATCH', body: JSON.stringify(body) }),
  deleteGroup: (id: number) => request<void>(`/v1/admin/groups/${id}`, { method: 'DELETE' }),
  tags: () => request<Tag[]>('/v1/admin/tags'),
  createTag: (name: string) =>
    request<Tag>('/v1/admin/tags', { method: 'POST', body: JSON.stringify({ name }) }),
  patchTag: (id: number, name: string) =>
    request<Tag>(`/v1/admin/tags/${id}`, { method: 'PATCH', body: JSON.stringify({ name }) }),
  deleteTag: (id: number) => request<void>(`/v1/admin/tags/${id}`, { method: 'DELETE' }),
  agents: () => request<Agent[]>('/v1/admin/agents'),
  patchAgent: (id: string, body: Partial<{ displayName: string; groupId: number; tagIds: number[] }>) =>
    request<Agent>(`/v1/admin/agents/${id}`, { method: 'PATCH', body: JSON.stringify(body) }),
  deleteAgent: (id: string) => request<void>(`/v1/admin/agents/${id}`, { method: 'DELETE' }),
  departments: () => request<Department[]>('/v1/admin/departments'),
  createDepartment: (body: { name: string; sortOrder?: number }) =>
    request<Department>('/v1/admin/departments', { method: 'POST', body: JSON.stringify(body) }),
  patchDepartment: (id: number, body: Partial<{ name: string; sortOrder: number }>) =>
    request<Department>(`/v1/admin/departments/${id}`, { method: 'PATCH', body: JSON.stringify(body) }),
  deleteDepartment: (id: number) =>
    request<void>(`/v1/admin/departments/${id}`, { method: 'DELETE' }),
  users: () => request<User[]>('/v1/admin/users'),
  createUser: (body: {
    username: string
    password: string
    role: UserRole
    departmentId: number
    enabled?: boolean
  }) => request<User>('/v1/admin/users', { method: 'POST', body: JSON.stringify(body) }),
  patchUser: (
    id: number,
    body: Partial<{ password: string; role: UserRole; departmentId: number; enabled: boolean }>,
  ) => request<User>(`/v1/admin/users/${id}`, { method: 'PATCH', body: JSON.stringify(body) }),
  deleteUser: (id: number) => request<void>(`/v1/admin/users/${id}`, { method: 'DELETE' }),
  downloadSecret: async (kind: 'agent-psk' | 'viewer-psk') => {
    const token = getToken()
    const res = await fetch(`/v1/admin/secrets/${kind}`, {
      headers: token ? { Authorization: `Bearer ${token}` } : {},
    })
    if (res.status === 401) {
      handleUnauthorized(401)
      throw new ApiError(401, '未授权')
    }
    if (!res.ok) {
      let msg = '下载失败'
      try {
        const data = await res.json()
        if (data?.error) msg = data.error
      } catch {
        /* ignore */
      }
      throw new ApiError(res.status, msg)
    }
    const blob = await res.blob()
    const a = document.createElement('a')
    a.href = URL.createObjectURL(blob)
    a.download = kind === 'agent-psk' ? 'agent-control.psk' : 'viewer.psk'
    document.body.appendChild(a)
    a.click()
    a.remove()
    setTimeout(() => URL.revokeObjectURL(a.href), 1000)
  },
}
