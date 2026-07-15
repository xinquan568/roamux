// SPDX-License-Identifier: Apache-2.0
// roam-140: the <roamux-update-card> template — the update surface only
// (status pill + Check Now + download/skip/restart). Identity + links moved to
// about_page.html.
import {html} from '//resources/lit/v3_0/lit.rollup.js';

import type {RoamuxUpdateCardElement} from './roamux_update_card.js';

export function getHtml(this: RoamuxUpdateCardElement) {
  // clang-format off
  return html`
    ${this.updatesAvailable_ ? html`
      <div class="check-row">
        ${this.isChecking_() ? html`
          <span id="spinner" class="spinner"></span>` : ''}
        <span id="statusPill" class="pill">${this.getStatusLabel_()}</span>
        <button id="checkNow" @click="${this.onCheckNowClick_}">Check Now</button>
      </div>

      ${this.isError_() ? html`
        <div id="errorText" class="error">${this.snapshot_.error}</div>` : ''}

      ${this.shouldShowCard_() ? html`
        <div id="updateCard" class="update-card">
          <div class="card-title">${this.getStatusLabel_()}</div>
          <div class="card-version">Version ${this.snapshot_.version}</div>
          <div class="card-date">${this.snapshot_.date}</div>
          <div class="card-notes">${this.snapshot_.notes}</div>

          ${this.isDownloading_() ? html`
            <progress id="progress" .value="${this.snapshot_.progress}">
            </progress>` : ''}

          ${this.isAvailable_() ? html`
            <button id="download" @click="${this.onDownloadClick_}">
              Download</button>
            <button id="skip" @click="${this.onSkipClick_}">
              Skip This Version</button>` : ''}

          ${this.isReady_() ? html`
            <button id="restart" @click="${this.onRestartClick_}">
              Restart to Update</button>` : ''}
        </div>` : ''}
    ` : html`
      <div id="updatesUnavailable" class="unavailable">
        Updates are not available in this build.
      </div>`}
  `;
  // clang-format on
}
