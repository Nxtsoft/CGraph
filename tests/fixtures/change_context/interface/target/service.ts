export interface Service {
  run(value: number): number;
}

export class Worker implements Service {
  run(value: number): number { return value; }
}

export function consume(s: Service): number {
  return s.run(2);
}
