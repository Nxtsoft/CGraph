export function normalize(path) {
  const [pathname, ...query] = path.split('?');
  const normalized = '/' + pathname.split('/').filter(Boolean).join('/');
  return normalized.toLowerCase() + (query.length ? '?' + query.join('?') : '');
}
