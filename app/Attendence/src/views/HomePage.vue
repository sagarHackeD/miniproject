<template>
  <ion-page>
    <ion-header :translucent="true">
      <ion-toolbar>
        <ion-title>Attendance</ion-title>
      </ion-toolbar>
    </ion-header>

    <ion-content :fullscreen="true">
      <ion-refresher slot="fixed" @ionRefresh="handlePullToRefresh">
        <ion-refresher-content
          pulling-text="Pull down to refresh"
          refreshing-spinner="crescent"
          refreshing-text="Refreshing attendance..."
        />
      </ion-refresher>
      <main class="dashboard">
        <section class="page-heading">
          <div>
            <p class="eyebrow">RFID ATTENDANCE</p>
            <h1>Attendance overview</h1>
            <p class="subtitle">A clear view of staff check-ins and check-outs.</p>
          </div>
          <button class="refresh-button" type="button" :disabled="isLoading" @click="loadAttendance">
            <span aria-hidden="true">{{ isLoading ? '…' : '↻' }}</span>
            {{ isLoading ? 'Loading' : 'Refresh' }}
          </button>
        </section>

        <form class="analytics-filters" @submit.prevent="loadAttendance">
          <label>
            <span>From</span>
            <input v-model="startDate" type="date" required />
          </label>
          <label>
            <span>To</span>
            <input v-model="endDate" type="date" required />
          </label>
          <button class="refresh-button" type="submit" :disabled="isLoading || !isDateRangeValid">Apply range</button>
          <span v-if="!isDateRangeValid" class="range-error" role="alert">
            Choose a valid range with the start date on or before the end date.
          </span>
        </form>

        <section class="summary-grid" aria-label="Attendance summary">
          <article class="summary-card">
            <p>Checked in now</p>
            <strong>{{ checkedInCount }}</strong>
            <span>Based on each staff member’s latest scan</span>
          </article>
          <article class="summary-card">
            <p>Scans in range</p>
            <strong>{{ todayCount }}</strong>
            <span>IN and OUT activity for the selected dates</span>
          </article>
          <article class="summary-card">
            <p>Staff in range</p>
            <strong>{{ staffSeenToday }}</strong>
            <span>Unique RFID cards scanned for the selected dates</span>
          </article>
        </section>

        <section class="analytics-grid" aria-label="Attendance analytics">
          <article class="analytics-card">
            <div class="analytics-card-heading">
              <div>
                <h2>Daily attendance trend</h2>
                <p>Scans and unique staff per day</p>
              </div>
            </div>
            <div class="daily-chart" role="list" aria-label="Daily scan counts">
              <div v-for="day in analytics?.daily ?? []" :key="day.date" class="chart-day" role="listitem">
                <span class="chart-count">{{ day.scans }}</span>
                <span class="chart-track">
                  <span
                    class="chart-bar"
                    :style="{ height: getBarHeight(day.scans) }"
                    :aria-label="`${day.scans} scans`"
                  ></span>
                </span>
                <span class="chart-staff">{{ day.staff_count }} staff</span>
                <span class="chart-date">{{ formatDayLabel(day.date) }}</span>
              </div>
            </div>
          </article>

          <article class="analytics-card">
            <div class="analytics-card-heading">
              <div>
                <h2>Staff summary</h2>
                <p>Attendance recorded during the selected range</p>
              </div>
            </div>
            <div class="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th scope="col">Staff member</th>
                    <th scope="col">Days with scans</th>
                    <th scope="col">Scans</th>
                    <th scope="col">First IN</th>
                    <th scope="col">Last OUT</th>
                  </tr>
                </thead>
                <tbody>
                  <tr v-for="person in analytics?.staff ?? []" :key="person.uid">
                    <td class="staff-name">{{ person.staff_name }}</td>
                    <td>{{ person.days_present }}</td>
                    <td>{{ person.scans }}</td>
                    <td class="timestamp">
                      {{ person.first_in ? formatTimestamp(person.first_in) : '—' }}
                    </td>
                    <td class="timestamp">
                      {{ person.last_out ? formatTimestamp(person.last_out) : '—' }}
                    </td>
                  </tr>
                  <tr v-if="analytics?.staff.length === 0">
                    <td colspan="5" class="empty-cell">No staff scans in this date range.</td>
                  </tr>
                </tbody>
              </table>
            </div>
          </article>
        </section>

        <section class="attendance-panel" aria-labelledby="activity-heading">
          <div class="panel-heading">
            <div>
              <h2 id="activity-heading">Attendance activity</h2>
              <p>Showing the latest {{ entries.length }} records.</p>
            </div>
            <div class="filters">
              <div class="period-switch" aria-label="Activity date filter">
                <button
                  type="button"
                  :class="{ selected: selectedPeriod === 'today' }"
                  :aria-pressed="selectedPeriod === 'today'"
                  @click="selectedPeriod = 'today'"
                >
                  Today
                </button>
                <button
                  type="button"
                  :class="{ selected: selectedPeriod === 'all' }"
                  :aria-pressed="selectedPeriod === 'all'"
                  @click="selectedPeriod = 'all'"
                >
                  All loaded
                </button>
              </div>
              <label class="search-box">
                <span class="sr-only">Search attendance records</span>
                <input v-model="searchTerm" type="search" placeholder="Search staff, UID or device" />
              </label>
            </div>
          </div>

          <div v-if="errorMessage" class="state-message error-message" role="alert">
            <strong>Could not load attendance</strong>
            <span>{{ errorMessage }}</span>
            <button type="button" @click="loadAttendance">Try again</button>
          </div>
          <div v-else-if="isLoading && entries.length === 0" class="state-message" role="status">
            <span class="loading-indicator" aria-hidden="true"></span>
            Loading attendance records…
          </div>
          <div v-else-if="filteredEntries.length === 0" class="state-message">
            <strong>{{ entries.length === 0 ? 'No attendance records yet' : 'No matching records' }}</strong>
            <span>
              {{
                entries.length === 0
                  ? 'Scans will appear here as soon as the device writes to Supabase.'
                  : 'Try a different search or date range.'
              }}
            </span>
          </div>
          <div v-else class="table-wrap">
            <table>
              <thead>
                <tr>
                  <th scope="col">Staff member</th>
                  <th scope="col">RFID UID</th>
                  <th scope="col">Device</th>
                  <th scope="col">Action</th>
                  <th scope="col">Time</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="entry in filteredEntries" :key="entry.id">
                  <td class="staff-name">{{ entry.staff_name }}</td>
                  <td class="identifier">{{ entry.uid }}</td>
                  <td>{{ entry.device_id }}</td>
                  <td>
                    <span class="action-badge" :class="entry.action.toLowerCase()">{{ entry.action }}</span>
                  </td>
                  <td class="timestamp">{{ formatTimestamp(entry.timestamp) }}</td>
                </tr>
              </tbody>
            </table>
          </div>
        </section>
        <p class="data-note">Data refreshes when you open this page or select Refresh.</p>
      </main>
    </ion-content>
  </ion-page>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import {
  IonContent,
  IonHeader,
  IonPage,
  IonRefresher,
  IonRefresherContent,
  IonTitle,
  IonToolbar,
  type RefresherCustomEvent,
} from '@ionic/vue';

import { getAttendanceDashboard, type AttendanceDashboard, type AttendanceRecord } from '@/services/attendance';

type Period = 'today' | 'all';

const rangeEnd = new Date();
const rangeStart = new Date(rangeEnd);
rangeStart.setDate(rangeStart.getDate() - 6);

const entries = ref<AttendanceRecord[]>([]);
const analytics = ref<AttendanceDashboard | null>(null);
const isLoading = ref(false);
const errorMessage = ref('');
const searchTerm = ref('');
const selectedPeriod = ref<Period>('today');
const startDate = ref(formatDateInput(rangeStart));
const endDate = ref(formatDateInput(rangeEnd));

const isDateRangeValid = computed(
  () => startDate.value !== '' && endDate.value !== '' && startDate.value <= endDate.value,
);

const checkedInCount = computed(() => analytics.value?.summary.checked_in_now ?? 0);
const todayCount = computed(() => analytics.value?.summary.scans_in_range ?? 0);
const staffSeenToday = computed(() => analytics.value?.summary.staff_in_range ?? 0);
const maxDailyScans = computed(
  () => analytics.value?.daily.reduce((maximum, day) => Math.max(maximum, day.scans), 1) ?? 1,
);

const filteredEntries = computed(() => {
  const query = searchTerm.value.trim().toLocaleLowerCase();

  return entries.value.filter((entry) => {
    const matchesPeriod = selectedPeriod.value === 'all' || isToday(entry.timestamp);
    const matchesSearch =
      query.length === 0 ||
      [entry.staff_name, entry.uid, entry.device_id, entry.action].some((value) =>
        value.toLocaleLowerCase().includes(query),
      );

    return matchesPeriod && matchesSearch;
  });
});

async function loadAttendance(): Promise<void> {
  if (isLoading.value || !isDateRangeValid.value) return;

  isLoading.value = true;
  errorMessage.value = '';

  try {
    const timeZone = Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC';
    const dashboard = await getAttendanceDashboard(startDate.value, endDate.value, timeZone);
    entries.value = dashboard.records;
    analytics.value = dashboard;
  } catch (error) {
    errorMessage.value = error instanceof Error ? error.message : 'An unexpected error occurred.';
  } finally {
    isLoading.value = false;
  }
}

async function handlePullToRefresh(event: RefresherCustomEvent): Promise<void> {
  try {
    await loadAttendance();
  } finally {
    await event.detail.complete();
  }
}

function formatDateInput(value: Date): string {
  const year = value.getFullYear();
  const month = String(value.getMonth() + 1).padStart(2, '0');
  const day = String(value.getDate()).padStart(2, '0');

  return `${year}-${month}-${day}`;
}

function isToday(value: string): boolean {
  return new Date(value).toDateString() === new Date().toDateString();
}

function formatDayLabel(value: string): string {
  const date = new Date(`${value}T00:00:00`);
  if (Number.isNaN(date.getTime())) return value;

  return new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric' }).format(date);
}

function formatTimestamp(value: string): string {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;

  return new Intl.DateTimeFormat(undefined, {
    dateStyle: 'medium',
    timeStyle: 'short',
  }).format(date);
}

function getBarHeight(scans: number): string {
  return scans === 0 ? '0%' : `${Math.max(4, (scans / maxDailyScans.value) * 100)}%`;
}

onMounted(() => void loadAttendance());
</script>

<style scoped>
ion-content {
  --background: #f5f8f6;
}

.dashboard {
  --ink: #172321;
  --muted: #75817e;
  --line: #e7ece9;
  --green: #18745c;
  --green-soft: #e6f4ee;

  width: min(1120px, 100%);
  margin: 0 auto;
  padding: 46px 28px 36px;
  color: var(--ink);
}

.page-heading,
.panel-heading {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: 24px;
}

.eyebrow {
  margin: 0 0 10px;
  color: var(--green);
  font-size: 0.72rem;
  font-weight: 750;
  letter-spacing: 0.14em;
}

h1,
h2,
p {
  margin-top: 0;
}

h1 {
  margin-bottom: 8px;
  font-size: clamp(1.8rem, 4vw, 2.45rem);
  letter-spacing: -0.045em;
}

.subtitle,
.panel-heading p {
  margin-bottom: 0;
  color: var(--muted);
}

.refresh-button {
  display: inline-flex;
  min-height: 42px;
  align-items: center;
  gap: 8px;
  padding: 0 16px;
  border: 1px solid #d5e5dd;
  border-radius: 10px;
  background: #fff;
  color: var(--green);
  cursor: pointer;
  font: inherit;
  font-size: 0.9rem;
  font-weight: 650;
}

.refresh-button:disabled {
  cursor: wait;
  opacity: 0.65;
}

.refresh-button span {
  font-size: 1.25rem;
}

.analytics-filters {
  display: grid;
  grid-template-columns: repeat(2, minmax(150px, 190px)) auto;
  align-items: end;
  gap: 12px;
  margin: 24px 0 0;
}

.analytics-filters label {
  display: grid;
  gap: 6px;
  color: var(--muted);
  font-size: 0.78rem;
}

.analytics-filters input {
  width: 100%;
  min-height: 42px;
  padding: 0 10px;
  border: 1px solid var(--line);
  border-radius: 9px;
  background: #fff;
  color: var(--ink);
  font: inherit;
}

.analytics-filters .refresh-button {
  justify-content: center;
}

.range-error {
  grid-column: 1 / -1;
  color: #a33434;
  font-size: 0.8rem;
}

.summary-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 16px;
  margin: 32px 0;
}

.summary-card {
  display: grid;
  min-height: 140px;
  align-content: start;
  gap: 8px;
  padding: 20px 22px;
  border: 1px solid var(--line);
  border-radius: 14px;
  background: #fff;
}

.summary-card p {
  margin: 0;
  color: var(--muted);
  font-size: 0.9rem;
}

.summary-card strong {
  font-size: 2rem;
  letter-spacing: -0.04em;
}

.summary-card span {
  color: var(--muted);
  font-size: 0.78rem;
}

.analytics-grid {
  display: grid;
  grid-template-columns: minmax(0, 0.9fr) minmax(0, 1.4fr);
  gap: 16px;
  margin-bottom: 32px;
}

.analytics-card {
  min-width: 0;
  overflow: hidden;
  border: 1px solid var(--line);
  border-radius: 14px;
  background: #fff;
}

.analytics-card-heading {
  padding: 19px 20px;
  border-bottom: 1px solid var(--line);
}

.analytics-card-heading h2 {
  margin: 0 0 5px;
}

.analytics-card-heading p {
  margin: 0;
  color: var(--muted);
  font-size: 0.82rem;
}

.daily-chart {
  display: grid;
  grid-auto-columns: minmax(42px, 1fr);
  grid-auto-flow: column;
  gap: 8px;
  overflow-x: auto;
  padding: 16px 18px 12px;
}

.chart-day {
  display: grid;
  height: 182px;
  grid-template-rows: 18px 1fr 16px 20px;
  align-items: end;
  color: var(--muted);
  font-size: 0.7rem;
  text-align: center;
}

.chart-count {
  font-variant-numeric: tabular-nums;
}

.chart-staff {
  font-size: 0.64rem;
  white-space: nowrap;
}

.chart-track {
  display: flex;
  height: 122px;
  align-items: flex-end;
  justify-content: center;
  border-bottom: 1px solid var(--line);
  background: linear-gradient(to bottom, transparent 49.5%, #eef1ef 50%, transparent 50.5%);
}

.chart-bar {
  width: 58%;
  border-radius: 5px 5px 0 0;
  background: var(--green);
}

.chart-date {
  align-self: end;
  white-space: nowrap;
}

.analytics-card table th,
.analytics-card table td {
  padding: 12px;
}

.empty-cell {
  color: var(--muted);
  text-align: center;
}

.attendance-panel {
  overflow: hidden;
  border: 1px solid var(--line);
  border-radius: 14px;
  background: #fff;
}

.panel-heading {
  align-items: center;
  padding: 22px 24px;
  border-bottom: 1px solid var(--line);
}

h2 {
  margin-bottom: 5px;
  font-size: 1.08rem;
}

.panel-heading p {
  font-size: 0.82rem;
}

.filters {
  display: flex;
  align-items: center;
  gap: 12px;
}

.period-switch {
  display: inline-flex;
  padding: 3px;
  border: 1px solid var(--line);
  border-radius: 9px;
  background: #f7f9f8;
}

.period-switch button {
  min-height: 32px;
  padding: 0 10px;
  border: 0;
  border-radius: 6px;
  background: transparent;
  color: var(--muted);
  cursor: pointer;
  font: inherit;
  font-size: 0.78rem;
  white-space: nowrap;
}

.period-switch button.selected {
  background: #fff;
  box-shadow: 0 1px 3px #16251f1a;
  color: var(--ink);
  font-weight: 650;
}

.search-box input {
  width: 220px;
  min-height: 40px;
  padding: 0 12px;
  border: 1px solid var(--line);
  border-radius: 9px;
  background: #fff;
  color: var(--ink);
  font: inherit;
  font-size: 0.82rem;
}

.search-box input:focus {
  border-color: #83b9a2;
  outline: 3px solid #18745c20;
}

.table-wrap {
  overflow-x: auto;
}

table {
  width: 100%;
  border-collapse: collapse;
  text-align: left;
  white-space: nowrap;
}

th,
td {
  padding: 15px 20px;
  border-bottom: 1px solid #eef1ef;
}

th {
  background: #fafbfa;
  color: var(--muted);
  font-size: 0.72rem;
  font-weight: 650;
}

td {
  font-size: 0.84rem;
}

tbody tr:last-child td {
  border-bottom: 0;
}

.staff-name {
  font-weight: 650;
}

.identifier,
.timestamp {
  color: var(--muted);
  font-variant-numeric: tabular-nums;
}

.action-badge {
  display: inline-flex;
  min-width: 46px;
  justify-content: center;
  padding: 5px 9px;
  border-radius: 999px;
  font-size: 0.72rem;
  font-weight: 750;
}

.action-badge.in {
  background: var(--green-soft);
  color: #17684f;
}

.action-badge.out {
  background: #f1f2f2;
  color: #5f6865;
}

.state-message {
  display: flex;
  min-height: 190px;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 28px;
  color: var(--muted);
  text-align: center;
}

.state-message strong {
  color: var(--ink);
}

.state-message span {
  font-size: 0.86rem;
}

.error-message button {
  margin-top: 8px;
  padding: 8px 12px;
  border: 0;
  border-radius: 8px;
  background: var(--green-soft);
  color: var(--green);
  cursor: pointer;
  font: inherit;
  font-weight: 650;
}

.loading-indicator {
  width: 22px;
  height: 22px;
  border: 2px solid #dce8e2;
  border-top-color: var(--green);
  border-radius: 50%;
  animation: spin 0.8s linear infinite;
}

.data-note {
  margin: 14px 0 0;
  color: var(--muted);
  font-size: 0.76rem;
  text-align: right;
}

.sr-only {
  position: absolute;
  width: 1px;
  height: 1px;
  overflow: hidden;
  clip: rect(0, 0, 0, 0);
  white-space: nowrap;
  clip-path: inset(50%);
}

@keyframes spin {
  to {
    transform: rotate(360deg);
  }
}

@media (max-width: 760px) {
  .dashboard {
    padding: 30px 16px;
  }

  .summary-grid {
    gap: 10px;
  }

  .summary-card {
    min-height: 128px;
    padding: 16px;
  }

  .summary-card span {
    line-height: 1.35;
  }

  .panel-heading {
    align-items: flex-start;
    flex-direction: column;
    padding: 20px 16px;
  }

  .analytics-filters {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .analytics-filters .refresh-button {
    grid-column: 1 / -1;
  }

  .analytics-grid {
    grid-template-columns: 1fr;
  }

  .filters {
    width: 100%;
    align-items: stretch;
    flex-direction: column;
  }

  .search-box input {
    width: 100%;
  }
}

@media (max-width: 480px) {
  .page-heading {
    align-items: flex-start;
  }

  .refresh-button {
    padding: 0 11px;
  }

  .summary-grid {
    grid-template-columns: 1fr;
    margin: 24px 0;
  }

  .analytics-filters {
    gap: 8px;
  }

  .summary-card {
    min-height: auto;
  }
}
</style>
