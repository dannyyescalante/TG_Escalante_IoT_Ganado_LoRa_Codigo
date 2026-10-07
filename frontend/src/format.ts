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
