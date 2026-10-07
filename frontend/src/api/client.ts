import type { Animal, Geofence, GeofenceEvent, LocationReading } from '../types'

// Vite reenvia /api al backend (ver vite.config.ts).
const API_BASE = '/api'

async function getJson<T>(path: string): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`)
  if (!response.ok) {
    throw new Error(`GET ${path} respondió ${response.status}`)
  }
  return response.json() as Promise<T>
}

export function getAnimals(): Promise<Animal[]> {
  return getJson<Animal[]>('/animals')
}

export async function getLatestLocation(animalId: number): Promise<LocationReading | null> {
  const readings = await getJson<LocationReading[]>(`/locations/${animalId}?limit=1`)
  return readings[0] ?? null
}

export function getGeofence(): Promise<Geofence> {
  return getJson<Geofence>('/geofence')
}

export function getEvents(): Promise<GeofenceEvent[]> {
  return getJson<GeofenceEvent[]>('/events')
}
