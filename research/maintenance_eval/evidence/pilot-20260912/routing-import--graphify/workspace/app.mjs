import { normalize } from './legacy.mjs';

export function routeRequest(path) {
  return { route: normalize(path) };
}

export function routeBatch(paths) {
  return paths.map(path => routeRequest(path));
}
