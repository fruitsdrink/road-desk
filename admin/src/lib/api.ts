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

export type ComputerRole = {
  id: number
  name: string
  sortOrder: number
  agentCount: number
  createdAt: string
}

export type ViewerPresence = {
  viewerId: string
  hostname: string
  username: string
  authMode: string
  version: string
  clientIp: string
  online: boolean
  lastSeenAt: string | null
  createdAt: string
  updatedAt: string
}

export type Agent = {
  agentId: string
  displayName: string
  hostname: string
  groupId: number
  mediaPort: number
  version: string
  preferredIpv4: string
  computerRole: string
  installLocation: string
  laneNumber: string
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

export type AuditSession = {
  id: string
  operatorUserId: number | null
  operatorName: string
  viewerHost: string
  viewerIp: string
  agentId: string
  agentName: string
  agentEndpoint: string
  mode: string
  result: string
  disconnectReason: string
  usedClipboard: boolean
  usedFileTransfer: boolean
  attemptedAt: string
  openedAt: string | null
  closedAt: string | null
  partial: boolean
  meta: Record<string, unknown>
  departmentName?: string
  createdAt: string
  updatedAt: string
}

export type AuditEvent = {
  id: number
  sessionId: string
  at: string
  source: string
  type: string
  detail: Record<string, unknown>
  createdAt: string
}

export type AuditSessionDetail = {
  session: AuditSession
  events: AuditEvent[]
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
  moveGroup: (id: number, body: { parentId: number | null; index: number }) =>
    request<Group>(`/v1/admin/groups/${id}/move`, { method: 'POST', body: JSON.stringify(body) }),
  deleteGroup: (id: number, opts?: { cascade?: boolean }) =>
    request<void>(
      `/v1/admin/groups/${id}${opts?.cascade ? '?cascade=1' : ''}`,
      { method: 'DELETE' },
    ),
  tags: () => request<Tag[]>('/v1/admin/tags'),
  createTag: (name: string) =>
    request<Tag>('/v1/admin/tags', { method: 'POST', body: JSON.stringify({ name }) }),
  patchTag: (id: number, name: string) =>
    request<Tag>(`/v1/admin/tags/${id}`, { method: 'PATCH', body: JSON.stringify({ name }) }),
  deleteTag: (id: number) => request<void>(`/v1/admin/tags/${id}`, { method: 'DELETE' }),
  computerRoles: () => request<ComputerRole[]>('/v1/admin/computer-roles'),
  createComputerRole: (body: { name: string; sortOrder?: number }) =>
    request<ComputerRole>('/v1/admin/computer-roles', {
      method: 'POST',
      body: JSON.stringify(body),
    }),
  patchComputerRole: (id: number, body: Partial<{ name: string; sortOrder: number }>) =>
    request<ComputerRole>(`/v1/admin/computer-roles/${id}`, {
      method: 'PATCH',
      body: JSON.stringify(body),
    }),
  deleteComputerRole: (id: number) =>
    request<void>(`/v1/admin/computer-roles/${id}`, { method: 'DELETE' }),
  viewers: () => request<ViewerPresence[]>('/v1/admin/viewers'),
  agents: () => request<Agent[]>('/v1/admin/agents'),
  patchAgent: (
    id: string,
    body: Partial<{
      displayName: string
      groupId: number
      computerRole: string
      installLocation: string
      laneNumber: string
      tagIds: number[]
    }>,
  ) => request<Agent>(`/v1/admin/agents/${id}`, { method: 'PATCH', body: JSON.stringify(body) }),
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
  auditSessions: (params: {
    from?: string
    to?: string
    agent_id?: string
    operator?: string
    result?: string
    department_id?: number
    limit?: number
    cursor?: string
  } = {}) => {
    const q = new URLSearchParams()
    for (const [k, v] of Object.entries(params)) {
      if (v != null && v !== '') q.set(k, String(v))
    }
    const qs = q.toString()
    return request<{ items: AuditSession[]; nextCursor?: string }>(
      `/v1/admin/audit/sessions${qs ? `?${qs}` : ''}`,
    )
  },
  auditSession: (id: string) =>
    request<AuditSessionDetail>(`/v1/admin/audit/sessions/${id}`),
  downloadAuditCsv: async (params: {
    from?: string
    to?: string
    agent_id?: string
    operator?: string
    result?: string
    department_id?: number
  } = {}) => {
    const token = getToken()
    const q = new URLSearchParams()
    for (const [k, v] of Object.entries(params)) {
      if (v != null && v !== '') q.set(k, String(v))
    }
    const qs = q.toString()
    const res = await fetch(`/v1/admin/audit/sessions/export${qs ? `?${qs}` : ''}`, {
      headers: token ? { Authorization: `Bearer ${token}` } : {},
    })
    if (res.status === 401) {
      handleUnauthorized(401)
      throw new ApiError(401, '未授权')
    }
    if (!res.ok) {
      let msg = '导出失败'
      try {
        const data = await res.json()
        if (data?.error) msg = data.error
      } catch {
        /* ignore */
      }
      throw new ApiError(res.status, msg)
    }
    const blob = await res.blob()
    const dispo = res.headers.get('Content-Disposition') || ''
    const match = /filename="?([^";]+)"?/i.exec(dispo)
    const a = document.createElement('a')
    a.href = URL.createObjectURL(blob)
    a.download = match?.[1] || 'audit-sessions.csv'
    document.body.appendChild(a)
    a.click()
    a.remove()
    URL.revokeObjectURL(a.href)
  },
  downloadAuditEventsCsv: async (params: {
    from?: string
    to?: string
    agent_id?: string
    operator?: string
    result?: string
    department_id?: number
    include_sensitive?: boolean
  } = {}) => {
    const token = getToken()
    const q = new URLSearchParams()
    for (const [k, v] of Object.entries(params)) {
      if (v == null || v === '') continue
      if (k === 'include_sensitive') {
        q.set(k, v ? '1' : '0')
        continue
      }
      q.set(k, String(v))
    }
    const qs = q.toString()
    const res = await fetch(`/v1/admin/audit/events/export${qs ? `?${qs}` : ''}`, {
      headers: token ? { Authorization: `Bearer ${token}` } : {},
    })
    if (res.status === 401) {
      handleUnauthorized(401)
      throw new ApiError(401, '未授权')
    }
    if (!res.ok) {
      let msg = '导出失败'
      try {
        const data = await res.json()
        if (data?.error) msg = data.error
      } catch {
        /* ignore */
      }
      throw new ApiError(res.status, msg)
    }
    const blob = await res.blob()
    const dispo = res.headers.get('Content-Disposition') || ''
    const match = /filename="?([^";]+)"?/i.exec(dispo)
    const a = document.createElement('a')
    a.href = URL.createObjectURL(blob)
    a.download = match?.[1] || 'audit-events.csv'
    document.body.appendChild(a)
    a.click()
    a.remove()
    URL.revokeObjectURL(a.href)
  },
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
