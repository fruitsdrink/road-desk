import { Store } from '@tanstack/react-store'

const TOKEN_KEY = 'road_desk_admin_token'

export function getToken(): string | null {
  return localStorage.getItem(TOKEN_KEY)
}

export function setToken(token: string | null) {
  if (token) localStorage.setItem(TOKEN_KEY, token)
  else localStorage.removeItem(TOKEN_KEY)
}

export const authStore = new Store<{ token: string | null }>({
  token: getToken(),
})

export function loginWithToken(token: string) {
  setToken(token)
  authStore.setState(() => ({ token }))
}

export function logout() {
  setToken(null)
  authStore.setState(() => ({ token: null }))
}

/** Clear session and go to login (e.g. after API 401). */
export function logoutToLogin() {
  logout()
  if (!window.location.pathname.endsWith('/login')) {
    window.location.assign('/login')
  }
}
