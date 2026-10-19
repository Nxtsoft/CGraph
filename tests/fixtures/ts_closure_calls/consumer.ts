import { Store } from './decoys';

// This file imports from decoys.ts, but not `write`: the `write` the closure
// calls is the parameter. A file-level import is no evidence for it.
export function persist(store: Store, write: (key: string) => void) {
  ['a'].forEach((key) => write(key));
  return store;
}
