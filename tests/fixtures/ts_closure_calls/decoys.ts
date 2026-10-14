// Project declarations that share a name with what service.ts's closures call
// through a parameter, a prop or a receiver. None of them is the callee.
export function onDone(): void {}

export function reject(reason: unknown): unknown {
  return reason;
}

export function onOpenChange(open: boolean): boolean {
  return open;
}

export class Store {
  delete(key: string): string {
    return key;
  }
}
