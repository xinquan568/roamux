// SPDX-License-Identifier: Apache-2.0
// roam-37: chrome://roamex-about WebUI tests against a TS-side fake proxy —
// the status matrix, Download→progress→Restart, Skip hides the card, NO
// configuration/reset groups, identity + links. (TDD/P6.)

import 'chrome://roamex-about/app.js';

import {RoamexAboutAppElement} from 'chrome://roamex-about/app.js';
import {BrowserProxyImpl} from 'chrome://roamex-about/browser_proxy.js';
import type {BrowserProxy} from 'chrome://roamex-about/browser_proxy.js';
import {UpdateStatus, type UpdateSnapshot} from 'chrome://roamex-about/update_page.mojom-webui.js';
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

suite('RoamexAbout', function() {
  let element: RoamexAboutAppElement;
  let fake: FakeProxy;

  setup(async function() {
    fake = new FakeProxy();
    // Inject before construction so no real Mojo bind ever fires (F3).
    BrowserProxyImpl.setInstance(fake);
    element = new RoamexAboutAppElement();
    element.setProxyForTesting(fake);
    document.body.appendChild(element);
    await element.updateComplete;
  });

  teardown(function() {
    element.remove();
  });

  function q(id: string): HTMLElement|null {
    return element.shadowRoot!.querySelector(`#${id}`);
  }

  test('identity and links render', function() {
    assertTrue(!!q('productName'));
    assertTrue(!!q('version'));
    assertTrue(!!q('websiteLink'));
    assertTrue(!!q('githubLink'));
  });

  test('no configuration or reset groups present', function() {
    // Termixion parity MINUS config: none of these exist.
    assertFalse(!!element.shadowRoot!.querySelector('settings-section'));
    assertFalse(!!q('resetGroup'));
    assertFalse(!!q('configGroup'));
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

  test('checking shows a spinner and pill text', async function() {
    fake.push({status: UpdateStatus.kChecking});
    await element.updateComplete;
    assertTrue(!!q('spinner'));
    assertEquals('Checking for updates…', q('statusPill')!.textContent!.trim());
    assertFalse(!!q('updateCard'));
  });

  test('up to date shows pill and no card', async function() {
    fake.push({status: UpdateStatus.kUpToDate});
    await element.updateComplete;
    assertEquals('Roamex is up to date', q('statusPill')!.textContent!.trim());
    assertFalse(!!q('updateCard'));
    assertFalse(!!q('spinner'));
  });

  test('available renders version, date and notes', async function() {
    fake.push({
      status: UpdateStatus.kAvailable,
      version: '2.0.0',
      date: '2026-07-01',
      notes: 'Shiny new build',
    });
    await element.updateComplete;
    assertTrue(q('updateCard')!.textContent!.includes('2.0.0'));
    assertTrue(q('updateCard')!.textContent!.includes('2026-07-01'));
    assertTrue(q('updateCard')!.textContent!.includes('Shiny new build'));
    // Available: download + skip present; restart + progress absent.
    assertTrue(!!q('download'));
    assertTrue(!!q('skip'));
    assertFalse(!!q('restart'));
    assertFalse(!!q('progress'));
  });

  test('downloading shows progress, not download buttons', async function() {
    fake.push({status: UpdateStatus.kDownloading, progress: 0.5});
    await element.updateComplete;
    assertTrue(!!q('progress'));
    assertFalse(!!q('download'));
    assertFalse(!!q('restart'));
  });

  test('ready shows only restart', async function() {
    fake.push({status: UpdateStatus.kReadyToInstall});
    await element.updateComplete;
    assertTrue(!!q('restart'));
    assertFalse(!!q('download'));
    assertFalse(!!q('progress'));
  });

  test('error renders error text and no card', async function() {
    fake.push({status: UpdateStatus.kError, error: 'network down'});
    await element.updateComplete;
    assertTrue(!!q('errorText'));
    assertEquals('network down', q('errorText')!.textContent!.trim());
    assertFalse(!!q('updateCard'));
  });
});
