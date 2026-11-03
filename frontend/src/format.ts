import { DISPLAY_TIME_ZONE } from './config'

const dateTimeFormat = new Intl.DateTimeFormat('es-VE', {
  timeZone: DISPLAY_TIME_ZONE,
  dateStyle: 'short',
  timeStyle: 'medium',
})

const timeFormat = new Intl.DateTimeFormat('es-VE', {
  timeZone: DISPLAY_TIME_ZONE,
  timeStyle: 'medium',
})

export function formatDateTime(value: string | Date): string {
  return dateTimeFormat.format(new Date(value))
}

export function formatTime(value: Date): string {
  return timeFormat.format(value)
}

// "hace 5 min", "hace 2 horas", "hace 6 días". Se recalcula en cada render (al menos cada 30 s, con cada consulta).
export function formatTimeAgo(value: string | Date, now: Date = new Date()): string {
  const minutes = Math.floor((now.getTime() - new Date(value).getTime()) / 60_000)
  if (minutes < 1) return 'hace menos de 1 min'
  if (minutes < 60) return `hace ${minutes} min`
  const hours = Math.floor(minutes / 60)
  if (hours < 24) return hours === 1 ? 'hace 1 hora' : `hace ${hours} horas`
  const days = Math.floor(hours / 24)
  return days === 1 ? 'hace 1 día' : `hace ${days} días`
}
