const TOKEN_KEY = 'road_desk_admin_token'

export function getToken(): string | null {
  return localStorage.getItem(TOKEN_KEY)
}

export function setToken(token: string | null) {
  if (token) localStorage.setItem(TOKEN_KEY, token)
  else localStorage.removeItem(TOKEN_KEY)
}

export class ApiError extends Error {
  status: number
  constructor(status: number, message: string) {
    super(message)
    this.status = status
  }
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
  if (!res.ok) {
    throw new ApiError(res.status, data?.error || res.statusText)
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

export type Tag = { id: number; name: string }

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

export const api = {
  login: (username: string, password: string) =>
    request<{ token: string }>('/v1/admin/login', {
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
  downloadSecret: async (kind: 'agent-psk' | 'viewer-psk') => {
    const token = getToken()
    const res = await fetch(`/v1/admin/secrets/${kind}`, {
      headers: token ? { Authorization: `Bearer ${token}` } : {},
    })
    if (!res.ok) throw new ApiError(res.status, 'download failed')
    const blob = await res.blob()
    const a = document.createElement('a')
    a.href = URL.createObjectURL(blob)
    a.download = kind === 'agent-psk' ? 'agent-control.psk' : 'viewer.psk'
    a.click()
    URL.revokeObjectURL(a.href)
  },
}
