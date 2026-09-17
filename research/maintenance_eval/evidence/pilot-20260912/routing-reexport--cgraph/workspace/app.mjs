import { normalizePath } from './facade.mjs';

export function routeRequest(path) {
  return { route: normalizePath(path) };
}

export function routeBatch(paths) {
  return paths.map(path => routeRequest(path));
}
