import { Store } from '@tanstack/react-store'
import { getToken, setToken } from './api'

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
