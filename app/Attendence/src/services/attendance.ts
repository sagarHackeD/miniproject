export interface AttendanceRecord {
  id: number;
  uid: string;
  staff_name: string;
  device_id: string;
  action: 'IN' | 'OUT';
  timestamp: string;
}

export interface DailyAttendanceSummary {
  date: string;
  scans: number;
  staff_count: number;
}

export interface StaffAttendanceSummary {
  uid: string;
  staff_name: string;
  days_present: number;
  scans: number;
  first_in: string | null;
  last_out: string | null;
  last_action: 'IN' | 'OUT';
  last_scan: string;
}

export interface AttendanceDashboard {
  records: AttendanceRecord[];
  summary: {
    checked_in_now: number;
    scans_in_range: number;
    staff_in_range: number;
  };
  daily: DailyAttendanceSummary[];
  staff: StaffAttendanceSummary[];
}

export async function getAttendanceDashboard(
  startDate: string,
  endDate: string,
  timeZone: string,
): Promise<AttendanceDashboard> {
  const supabaseUrl = import.meta.env.VITE_SUPABASE_URL?.trim().replace(/\/+$/, '');
  const supabaseAnonKey = import.meta.env.VITE_SUPABASE_ANON_KEY?.trim();

  if (!supabaseUrl || !supabaseAnonKey) {
    throw new Error(
      'Add VITE_SUPABASE_URL and VITE_SUPABASE_ANON_KEY to app/Attendence/.env.local, then restart the app.',
    );
  }

  let response: Response;

  try {
    response = await fetch(`${supabaseUrl}/rest/v1/rpc/get_attendance_dashboard`, {
      method: 'POST',
      headers: {
        apikey: supabaseAnonKey,
        Authorization: `Bearer ${supabaseAnonKey}`,
        Accept: 'application/json',
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({
        p_start_date: startDate,
        p_end_date: endDate,
        p_timezone: timeZone,
      }),
    });
  } catch (error) {
    throw new Error(
      `Could not connect to Supabase: ${error instanceof Error ? error.message : 'network request failed.'}`,
      { cause: error },
    );
  }

  if (!response.ok) {
    const details = (await response.text()).trim();
    throw new Error(
      `Supabase returned ${response.status}${details ? `: ${details}` : '. Check the project URL, API key, and attendance read policy.'}`,
    );
  }

  const data: unknown = await response.json();
  if (!isAttendanceDashboard(data)) {
    throw new Error('Supabase returned attendance analytics in an unexpected format.');
  }

  return data;
}

function isAttendanceDashboard(value: unknown): value is AttendanceDashboard {
  if (!isObject(value)) return false;

  return (
    Array.isArray(value.records) &&
    value.records.every(isAttendanceRecord) &&
    isObject(value.summary) &&
    isCount(value.summary.checked_in_now) &&
    isCount(value.summary.scans_in_range) &&
    isCount(value.summary.staff_in_range) &&
    Array.isArray(value.daily) &&
    value.daily.every(isDailyAttendanceSummary) &&
    Array.isArray(value.staff) &&
    value.staff.every(isStaffAttendanceSummary)
  );
}

function isAttendanceRecord(value: unknown): value is AttendanceRecord {
  if (!isObject(value)) return false;

  return (
    isCount(value.id) &&
    typeof value.uid === 'string' &&
    typeof value.staff_name === 'string' &&
    typeof value.device_id === 'string' &&
    (value.action === 'IN' || value.action === 'OUT') &&
    isTimestamp(value.timestamp)
  );
}

function isDailyAttendanceSummary(value: unknown): value is DailyAttendanceSummary {
  if (!isObject(value)) return false;

  return (
    typeof value.date === 'string' &&
    /^\d{4}-\d{2}-\d{2}$/.test(value.date) &&
    isCount(value.scans) &&
    isCount(value.staff_count)
  );
}

function isStaffAttendanceSummary(value: unknown): value is StaffAttendanceSummary {
  if (!isObject(value)) return false;

  return (
    typeof value.uid === 'string' &&
    typeof value.staff_name === 'string' &&
    isCount(value.days_present) &&
    isCount(value.scans) &&
    isNullableTimestamp(value.first_in) &&
    isNullableTimestamp(value.last_out) &&
    (value.last_action === 'IN' || value.last_action === 'OUT') &&
    isTimestamp(value.last_scan)
  );
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function isCount(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;
}

function isTimestamp(value: unknown): value is string {
  return typeof value === 'string' && !Number.isNaN(Date.parse(value));
}

function isNullableTimestamp(value: unknown): value is string | null {
  return value === null || isTimestamp(value);
}
