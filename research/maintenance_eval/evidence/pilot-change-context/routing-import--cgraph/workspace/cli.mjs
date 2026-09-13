import { routeRequest } from './app.mjs';

export function main(path) {
  return JSON.stringify(routeRequest(path));
}
