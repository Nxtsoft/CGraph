export interface Service {
  run(): number;
}

export class Worker implements Service {
  run(): number { return 1; }
}

export function consume(s: Service): number {
  return s.run();
}
