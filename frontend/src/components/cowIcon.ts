import { divIcon, type DivIcon } from 'leaflet'
import { STATUS_COLORS } from '../config'

// Silueta de cabeza de vaca dibujada en SVG (sin imagenes externas ni emojis, que cambian segun el dispositivo).
const COW_SVG = `
<svg viewBox="0 0 32 32" width="22" height="22" aria-hidden="true" focusable="false">
  <path d="M9.5 6.2C9 4.4 9.6 2.7 11 1.8c.2 1.6-.1 3-.8 4.4zM22.5 6.2c.5-1.8-.1-3.5-1.5-4.4-.2 1.6.1 3 .8 4.4z" fill="#f3e3c3"/>
  <path d="M7.4 9.6C4.9 9.4 3 8 2.4 6.4c2.2-.3 4.3.3 6 1.4zM24.6 9.6c2.5-.2 4.4-1.6 5-3.2-2.2-.3-4.3.3-6 1.4z" fill="#ffffff"/>
  <path d="M8 8.6C8 6.3 11.6 4.6 16 4.6s8 1.7 8 4V16c0 2-.9 3.2-.9 5.2H8.9C8.9 19.2 8 18 8 16z" fill="#ffffff"/>
  <path d="M17.2 5.2c2.3 0 4.2 1.4 4.3 3.3-2 .5-4.3-.5-4.3-3.3z" fill="#2b2b2b"/>
  <ellipse cx="16" cy="23.2" rx="8.2" ry="6" fill="#f4bcc8"/>
  <ellipse cx="12.9" cy="23.6" rx="1.3" ry="1.9" fill="#7a3b4a"/>
  <ellipse cx="19.1" cy="23.6" rx="1.3" ry="1.9" fill="#7a3b4a"/>
  <circle cx="12" cy="13" r="1.7" fill="#1f2933"/>
  <circle cx="20" cy="13" r="1.7" fill="#1f2933"/>
</svg>`

const cache = new Map<boolean, DivIcon>()

// Circulo verde (dentro) o rojo (salida confirmada) con la vaca dentro. La etiqueta con el tag
// la pone un Tooltip permanente en HerdMap.
export function cowIcon(outside: boolean): DivIcon {
  let icon = cache.get(outside)
  if (!icon) {
    const color = outside ? STATUS_COLORS.outside : STATUS_COLORS.inside
    icon = divIcon({
      className: 'cow-marker',
      html: `<div class="cow-marker__pin" style="background:${color}">${COW_SVG}</div>`,
      iconSize: [36, 36],
      iconAnchor: [18, 18],
      popupAnchor: [0, -18],
      tooltipAnchor: [0, 18],
    })
    cache.set(outside, icon)
  }
  return icon
}
