import { describe, expect, it } from 'vitest';
import { assertAccess } from './access';

describe('assertAccess', () => {
  it('returns the id', async () => {
    expect(await assertAccess('a')).toBe('a');
  });
});
