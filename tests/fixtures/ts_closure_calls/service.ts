import { Effect, Layer } from 'effect';
import { assertAccess, audit } from './access';

// Closures written into a module-level binding: the Effect `try:` / `catch:`
// bodies and the transaction callback inside them belong to `ServiceLive`.
export const ServiceLive = Layer.succeed(ServiceTag, {
  merge: (id: string) => Effect.tryPromise({
    try: async () => {
      await assertAccess(id);
      return db.transaction(async (tx) => audit(tx.name));
    },
    catch: (error) => toError(error),
  }),
});

function toError(error: unknown): Error {
  return new Error(String(error));
}

// Closures inside a named function belong to that function.
export function load(ids: string[]) {
  return Promise.all(ids.map((id) => assertAccess(id))).then(() => audit('load'));
}

// A class-field arrow belongs to its class.
export class Handler {
  handle = (id: string) => assertAccess(id);
}

// Names a closure is handed -- a parameter, a destructured prop, a receiver of
// unknown type. decoys.ts declares each name, and this file does not import it.
export function wait(onDone: () => void) {
  return new Promise<void>((resolve, reject) => {
    setTimeout(() => {
      onDone();
      reject(resolve);
    }, 0);
  });
}

export const Dialog = ({ onOpenChange }: { onOpenChange: (open: boolean) => void }) => {
  const close = () => onOpenChange(false);
  return close;
};

const cache = new Map<string, string>();

export function evict(keys: string[]) {
  keys.forEach((key) => cache.delete(key));
}
