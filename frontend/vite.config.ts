import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// El backend no tiene CORS: en desarrollo el navegador llama a /api (mismo origen)
// y Vite reenvia la peticion al backend.
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': {
        target: 'http://localhost:3018',
        rewrite: (path) => path.replace(/^\/api/, ''),
      },
    },
  },
})
