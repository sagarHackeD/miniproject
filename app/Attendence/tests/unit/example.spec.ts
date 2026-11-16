import { flushPromises, mount } from '@vue/test-utils';
import { afterEach, describe, expect, test, vi } from 'vitest';

import HomePage from '@/views/HomePage.vue';

describe('HomePage.vue', () => {
  afterEach(() => {
    vi.unstubAllEnvs();
    vi.unstubAllGlobals();
  });

  test('loads Supabase attendance and filters the activity table', async () => {
    const now = new Date().toISOString();
    const today = now.slice(0, 10);
    const dashboardResponse = {
      records: [
        {
          id: 2,
          uid: 'a1b2c3',
          staff_name: 'Jordan Lee',
          device_id: 'reader-1',
          action: 'IN',
          timestamp: now,
        },
        {
          id: 1,
          uid: 'd4e5f6',
          staff_name: 'Avery Kim',
          device_id: 'reader-2',
          action: 'OUT',
          timestamp: now,
        },
      ],
      summary: {
        checked_in_now: 1,
        scans_in_range: 2,
        staff_in_range: 2,
      },
      daily: [{ date: today, scans: 2, staff_count: 2 }],
      staff: [
        {
          uid: 'a1b2c3',
          staff_name: 'Jordan Lee',
          days_present: 1,
          scans: 1,
          first_in: now,
          last_out: null,
          last_action: 'IN',
          last_scan: now,
        },
        {
          uid: 'd4e5f6',
          staff_name: 'Avery Kim',
          days_present: 1,
          scans: 1,
          first_in: null,
          last_out: now,
          last_action: 'OUT',
          last_scan: now,
        },
      ],
    };
    const fetchMock = vi.fn().mockImplementation(() =>
      Promise.resolve(
        new Response(JSON.stringify(dashboardResponse), {
          headers: { 'Content-Type': 'application/json' },
        }),
      ),
    );

    vi.stubEnv('VITE_SUPABASE_URL', 'https://project.supabase.co');
    vi.stubEnv('VITE_SUPABASE_ANON_KEY', 'public-anon-key');
    vi.stubGlobal('fetch', fetchMock);

    const wrapper = mount(HomePage);
    await flushPromises();

    expect(fetchMock).toHaveBeenCalledWith(
      expect.stringContaining('/rest/v1/rpc/get_attendance_dashboard'),
      expect.objectContaining({
        method: 'POST',
        headers: expect.objectContaining({
          apikey: 'public-anon-key',
          'Content-Type': 'application/json',
        }),
      }),
    );
    expect(wrapper.text()).toContain('Attendance overview');
    expect(wrapper.text()).toContain('Jordan Lee');
    expect(wrapper.text()).toContain('Daily attendance trend');
    expect(wrapper.text()).toContain('Staff summary');
    expect(wrapper.text()).toContain('Checked in now');
    expect(wrapper.get('.summary-card:nth-child(2) strong').text()).toBe('2');
    expect(wrapper.get('button.refresh-button').text()).toContain('Refresh');

    const completeRefresh = vi.fn();
    await wrapper.find('ion-refresher').trigger('ionRefresh', {
      detail: { complete: completeRefresh },
    });
    await flushPromises();

    expect(completeRefresh).toHaveBeenCalledOnce();
    expect(fetchMock).toHaveBeenCalledTimes(2);

    await wrapper.get('input[type="search"]').setValue('reader-2');
    expect(wrapper.find('.attendance-panel tbody').text()).toContain('Avery Kim');
    expect(wrapper.find('.attendance-panel tbody').text()).not.toContain('Jordan Lee');

    wrapper.unmount();
  });
});
