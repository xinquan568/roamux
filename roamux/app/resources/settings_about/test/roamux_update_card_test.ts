// SPDX-License-Identifier: Apache-2.0
// roam-140: <roamux-update-card> WebUI tests against a TS-side fake proxy — the
// status matrix, Download→progress→Restart, Skip hides the card, and the static
// "updates unavailable" state. Adapted from the retired chrome://roamux-about
// about_test.ts (roam-37); identity + links assertions moved to the settings
// about_page integration suite (roamux_settings_about_test.ts). (TDD/P6.)

// The element pulls in strings.m.js (roam-136), so loadTimeData carries the REAL
// data-source values (updatesAvailable) by the time the element is constructed.
// This suite deliberately does NOT import strings.m.js on its own — doing so
// masked roam-136 (the production card renders blank when the element forgets
// the import), so the suite genuinely exercises the production bootstrap.
import 'chrome://settings/roamux_about/roamux_update_card.js';

import {RoamuxUpdateCardElement} from 'chrome://settings/roamux_about/roamux_update_card.js';
import {BrowserProxyImpl} from 'chrome://settings/roamux_about/browser_proxy.js';
import type {BrowserProxy} from 'chrome://settings/roamux_about/browser_proxy.js';
import {UpdateStatus} from 'chrome://settings/roamux_about/update_page.mojom-webui.js';
import type {UpdateSnapshot} from 'chrome://settings/roamux_about/update_page.mojom-webui.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {assertEquals, assertTrue, assertFalse} from 'chrome://webui-test/chai_assert.js';

// A TS-side fake UpdatePageHandler + a snapshot pump (no C++/Mojo needed).
class FakeProxy implements BrowserProxy {
  checkCount = 0;
  downloadCount = 0;
  installCount = 0;
  skipped: string[] = [];
  private listener_: ((s: UpdateSnapshot) => void)|null = null;

  handler = {
    checkForUpdates: () => {
      this.checkCount++;
    },
    download: () => {
      this.downloadCount++;
    },
    installAndRelaunch: () => {
      this.installCount++;
    },
    skip: (v: string) => {
      this.skipped.push(v);
    },
  } as unknown as BrowserProxy['handler'];

  callbackRouter = {
    onStateChanged: {
      addListener: (cb: (s: UpdateSnapshot) => void) => {
        this.listener_ = cb;
      },
    },
  } as unknown as BrowserProxy['callbackRouter'];

  push(snapshot: Partial<UpdateSnapshot>) {
    this.listener_!({
      status: UpdateStatus.kIdle,
      version: '',
      date: '',
      notes: '',
      error: '',
      progress: 0,
      ...snapshot,
    });
  }
}

suite('RoamuxUpdateCard', function() {
  let element: RoamuxUpdateCardElement;
  let fake: FakeProxy;

  setup(async function() {
    fake = new FakeProxy();
    // Force the update-card path on so the fake's onStateChanged listener is
    // registered at connect. In a non-sparkle test build the real
    // updatesAvailable is false, which would leave the card (and every card
    // test) inert.
    loadTimeData.overrideValues({updatesAvailable: true});
    // Inject before construction so no real Mojo bind ever fires (F3).
    BrowserProxyImpl.setInstance(fake);
    element = new RoamuxUpdateCardElement();
    element.setProxyForTesting(fake);
    document.body.appendChild(element);
    await element.updateComplete;
  });

  teardown(function() {
    element.remove();
  });

  function q(id: string): HTMLElement|null {
    return element.shadowRoot.querySelector(`#${id}`);
  }

  test('no configuration or reset groups present', function() {
    // The card is the update surface ONLY — no settings/reset/config groups,
    // and (post roam-140) no identity or links (those live in about_page.html).
    assertFalse(!!element.shadowRoot.querySelector('settings-section'));
    assertFalse(!!q('resetGroup'));
    assertFalse(!!q('configGroup'));
    assertFalse(!!q('productName'));
    assertFalse(!!q('websiteLink'));
    assertFalse(!!q('githubLink'));
  });

  test('available shows card with download and skip', async function() {
    fake.push({status: UpdateStatus.kAvailable, version: '2.0.0'});
    await element.updateComplete;
    assertTrue(!!q('updateCard'));
    assertTrue(!!q('download'));
    assertTrue(!!q('skip'));
  });

  test('download then progress then restart', async function() {
    fake.push({status: UpdateStatus.kAvailable, version: '2.0.0'});
    await element.updateComplete;
    q('download')!.click();
    assertEquals(1, fake.downloadCount);

    fake.push({status: UpdateStatus.kDownloading, progress: 0.5});
    await element.updateComplete;
    assertTrue(!!q('progress'));

    fake.push({status: UpdateStatus.kReadyToInstall});
    await element.updateComplete;
    assertTrue(!!q('restart'));
    q('restart')!.click();
    assertEquals(1, fake.installCount);
  });

  test('skip hides the card', async function() {
    fake.push({status: UpdateStatus.kAvailable, version: '2.0.0'});
    await element.updateComplete;
    q('skip')!.click();
    assertEquals(1, fake.skipped.length);
    assertEquals('2.0.0', fake.skipped[0]);

    // The service would push upToDate after a skip → card gone.
    fake.push({status: UpdateStatus.kUpToDate});
    await element.updateComplete;
    assertFalse(!!q('updateCard'));
  });

  test('check now issues a check', function() {
    q('checkNow')!.click();
    assertEquals(1, fake.checkCount);
  });

  // Table-driven status matrix: for every UpdateStatus, assert the exact pill
  // text and the FULL set of present/absent controls (F4).
  interface Row {
    status: UpdateStatus;
    version?: string;
    date?: string;
    notes?: string;
    error?: string;
    pill: string;
    present: string[];
    absent: string[];
  }
  const MATRIX: Row[] = [
    {
      status: UpdateStatus.kChecking,
      pill: 'Checking for updates…',
      present: ['spinner', 'statusPill', 'checkNow'],
      absent: ['updateCard', 'download', 'skip', 'restart', 'progress',
               'errorText'],
    },
    {
      status: UpdateStatus.kUpToDate,
      pill: 'Roamux is up to date',
      present: ['statusPill', 'checkNow'],
      absent: ['spinner', 'updateCard', 'download', 'skip', 'restart',
               'progress', 'errorText'],
    },
    {
      status: UpdateStatus.kAvailable,
      version: '2.0.0',
      date: '2026-07-01',
      notes: 'Shiny new build',
      pill: 'Update available',
      present: ['statusPill', 'updateCard', 'download', 'skip'],
      absent: ['spinner', 'restart', 'progress', 'errorText'],
    },
    {
      status: UpdateStatus.kDownloading,
      pill: 'Downloading…',
      present: ['statusPill', 'updateCard', 'progress'],
      absent: ['spinner', 'download', 'skip', 'restart', 'errorText'],
    },
    {
      status: UpdateStatus.kReadyToInstall,
      pill: 'Ready to install',
      present: ['statusPill', 'updateCard', 'restart'],
      absent: ['spinner', 'download', 'skip', 'progress', 'errorText'],
    },
    {
      status: UpdateStatus.kError,
      error: 'network down',
      pill: 'network down',
      present: ['statusPill', 'errorText'],
      absent: ['spinner', 'updateCard', 'download', 'skip', 'restart',
               'progress'],
    },
  ];

  for (const row of MATRIX) {
    test(`status ${row.status}: pill text + controls`, async function() {
      fake.push({
        status: row.status,
        version: row.version ?? '',
        date: row.date ?? '',
        notes: row.notes ?? '',
        error: row.error ?? '',
        progress: row.status === UpdateStatus.kDownloading ? 0.5 : 0,
      });
      await element.updateComplete;

      assertEquals(row.pill, q('statusPill')!.textContent.trim());
      for (const id of row.present) {
        assertTrue(!!q(id), `expected #${id} present for status ${row.status}`);
      }
      for (const id of row.absent) {
        assertFalse(!!q(id), `expected #${id} absent for status ${row.status}`);
      }

      if (row.version) {
        assertTrue(q('updateCard')!.textContent.includes(row.version));
      }
      if (row.date) {
        assertTrue(q('updateCard')!.textContent.includes(row.date));
      }
      if (row.notes) {
        assertTrue(q('updateCard')!.textContent.includes(row.notes));
      }
      if (row.error) {
        assertEquals(row.error, q('errorText')!.textContent.trim());
      }
    });
  }

  test('updates unavailable renders the static state', async function() {
    // Reconstruct with updatesAvailable=false: the card must render the static
    // "unavailable" note and NEVER register a Mojo listener / issue a check.
    element.remove();
    loadTimeData.overrideValues({updatesAvailable: false});
    const offline = new RoamuxUpdateCardElement();
    document.body.appendChild(offline);
    await offline.updateComplete;
    assertTrue(!!offline.shadowRoot.querySelector('#updatesUnavailable'));
    assertFalse(!!offline.shadowRoot.querySelector('#checkNow'));
    offline.remove();
  });
});
