import { describe, expect, it } from 'vitest';
import { assertAccess, audit } from './access';

describe('assertAccess', () => {
  it('returns the id', async () => {
    expect(await assertAccess('a')).toBe('a');
  });

  it('audits', function () {
    expect(audit('x')).toBe('x');
  });
});
