import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: { '@': '/src' },
  },
  server: {
    port: 5173,
    proxy: {
      '/v1': 'http://127.0.0.1:8743',
      '/healthz': 'http://127.0.0.1:8743',
    },
  },
  build: {
    outDir: '../tools/gateway/web/dist',
    emptyOutDir: true,
  },
})
