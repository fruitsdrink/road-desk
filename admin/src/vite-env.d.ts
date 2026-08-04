/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_DEV_ADMIN_PASSWORD?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
