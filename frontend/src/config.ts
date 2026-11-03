// Los collares transmiten cada 60 s (RF8): consultar mas seguido no mostraria datos nuevos.
export const POLL_INTERVAL_MS = 30_000

export const DISPLAY_TIME_ZONE = 'America/Caracas'

export const STATUS_COLORS = {
  inside: '#2e7d32',
  outside: '#c62828',
  geofence: '#1565c0',
} as const

// Al encuadrar el mapa solo se incluyen los animales cercanos a la geocerca: una lectura vieja lejana
// (p. ej. de otra prueba) alejaria tanto el mapa que el potrero no se veria.
export const FIT_MAX_DISTANCE_M = 1000

// Capas base del mapa. La satelital va por defecto; su atribucion es obligatoria.
export const BASE_LAYERS = {
  satellite: {
    name: 'Satélite',
    url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}',
    attribution:
      'Tiles &copy; Esri &mdash; Source: Esri, Maxar, Earthstar Geographics, and the GIS User Community',
    // Donde no hay imagen a mas zoom, se amplia la ultima disponible en vez de mostrar mosaicos vacios.
    maxNativeZoom: 18,
  },
  streets: {
    name: 'Mapa (OpenStreetMap)',
    url: 'https://tile.openstreetmap.org/{z}/{x}/{y}.png',
    attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
    maxNativeZoom: 19,
  },
} as const

export const MAP_MAX_ZOOM = 20
