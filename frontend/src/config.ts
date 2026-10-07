// Los collares transmiten cada 60 s (RF8): consultar mas seguido no mostraria datos nuevos.
export const POLL_INTERVAL_MS = 30_000

export const DISPLAY_TIME_ZONE = 'America/Caracas'

export const STATUS_COLORS = {
  inside: '#2e7d32',
  outside: '#c62828',
  geofence: '#1565c0',
} as const
