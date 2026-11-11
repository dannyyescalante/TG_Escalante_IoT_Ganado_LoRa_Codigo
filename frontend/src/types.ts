import type { Polygon } from 'geojson'

// Formas exactas de las respuestas del backend (GET /animals, /locations/:animalId, /geofence).

export interface Animal {
  id: number
  tag_number: string
  name: string | null
  breed: string | null
  collar_device_id: string
  collar_installed_at: string | null
  is_active: boolean
  created_at: string
  updated_at: string
}

export interface LocationReading {
  id: string
  animal_id: number
  lat: number
  lon: number
  recorded_at: string
  received_at: string
  inside_geofence: boolean
  created_at: string
}

export interface Geofence {
  id: number
  name: string
  geometry: Polygon
}

// GET /events (ordenado por occurred_at DESC, con JOIN a animals).
export interface GeofenceEvent {
  id: string
  animal_id: number
  tag_number: string
  animal_name: string | null
  location_id: string
  geofence_id: number
  event_type: 'exit' | 'enter'
  occurred_at: string
  created_at: string
}
