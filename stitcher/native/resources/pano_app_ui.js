/**
 * @typedef {'input' | 'preview' | 'output'} WorkflowStage
 */

/**
 * @typedef {'complete' | 'incomplete' | 'invalid' | 'stitched'} SessionStatus
 */

/**
 * @typedef {Object} PanoramaSessionSnapshot
 * @property {string} name Human-readable local session label.
 * @property {number} poses Number of captured poses.
 * @property {string} tag User-assigned tag, or an empty string.
 * @property {SessionStatus} status
 * @property {string} detail Validation details, or an empty string.
 * @property {boolean} hasCoordinates Whether the session has a saved location.
 */

/**
 * Complete authoritative state sent from the native application.
 *
 * @typedef {Object} PanoramaSnapshot
 * @property {1} version Native/WebView protocol version.
 * @property {'snapshot'} kind Message discriminator.
 * @property {number} pageGeneration Current HTML document generation.
 * @property {number} layoutGeneration Current preview-layout generation.
 * @property {boolean} maximized Whether the native window is maximized.
 * @property {WorkflowStage} stage
 * @property {string} gameDirectory
 * @property {string} screenshotsDirectory
 * @property {number | null} selectedIndex Index into `sessions`.
 * @property {string} status User-facing native status text.
 * @property {boolean} busy Whether a preview operation is running.
 * @property {boolean} previewEnabled Whether Preview may be started.
 * @property {boolean} previewReady Whether a retained preview exists.
 * @property {boolean} exposureOpen Whether the exposure satellite is visible.
 * @property {boolean} exposureAdjusted Whether the retained preview has non-default exposure gains.
 * @property {number} previewProgress Combined progress from 0 through 100.
 * @property {string} previewMessage Preview placeholder text.
 * @property {string} outputDirectory
 * @property {string} outputName
 * @property {boolean} resolutionPixels
 * @property {string} resolutionPercent
 * @property {string} outputWidth
 * @property {number} outputMaximumWidth Maximum pixel width at 100% scale.
 * @property {string} outputSummary
 * @property {'jpeg' | 'png' | 'exr'} outputFormat
 * @property {string} jpegQuality
 * @property {boolean} renderEnabled
 * @property {boolean} rendering
 * @property {number} outputProgress Combined render progress from 0 through 100.
 * @property {boolean} outputComplete Whether the current preview has a published render.
 * @property {PanoramaModalSnapshot | null} modal Native-authoritative modal state.
 * @property {PanoramaSessionSnapshot[]} sessions
 */

/**
 * @typedef {Object} PanoramaModalSnapshot
 * @property {number} generation Monotonic modal instance generation.
 * @property {string} kind Allow-listed native modal kind.
 * @property {boolean} dismissible Whether Escape, backdrop, and Close dismiss it.
 * @property {{
 *   title: string,
 *   description: string,
 *   value: string,
 *   error: string,
 *   charactersRemaining: number,
 *   canSubmit: boolean,
 *   readOnly: boolean,
 *   checked: boolean
 * }} payload
 */

/**
 * @typedef {Object} PanoramaSnapshotEvent
 * @property {PanoramaSnapshot} data
 */

/**
 * @typedef {Object} NativeWebViewBridge
 * @property {
 *   (type: 'message',
 *    listener: (event: PanoramaSnapshotEvent) => void) => void
 * } addEventListener
 * @property {(message: unknown) => void} postMessage
 */

(() => {
  'use strict';

  const protocolVersion = 1;
  const renderedProperties = new WeakMap();
  const renderedEvents = new WeakMap();

  /** @type {{ parent: Node, index: number } | null} */
  let renderCursor = null;

  const runtime = {
    /** @type {PanoramaSnapshot | null} */
    snapshot: null,
    pageGeneration: 0,
    geometryFrame: 0,
    contentSizeFrame: 0,
    lastContentHeight: null,
    lastContentLayoutGeneration: null,
    naturalContentHeight: null,
    lastPreviewGeometry: null,
    deviceScale: window.devicePixelRatio,
    bridgeFailed: false,
    /** @type {HTMLElement | null} */
    modalReturnFocus: null,
    /** @type {NativeWebViewBridge | null} */
    webview: null
  };

  /**
   * @param {string} id
   * @returns {HTMLElement}
   */
  function byId(id) {
    const element = document.getElementById(id);
    if (!element) throw new Error(`Missing UI element: ${id}`);
    return element;
  }

  /** @returns {{ parent: Node, index: number }} */
  function cursor() {
    if (!renderCursor) throw new Error('UI element rendered outside draw()');
    return renderCursor;
  }

  /** @param {Element} element @param {string} name */
  function removeProperty(element, name) {
    if (name.startsWith('on')) {
      const events = renderedEvents.get(element);
      const registration = events?.get(name.slice(2).toLowerCase());
      if (registration) registration.handler = null;
      return;
    }
    if (name === 'className') {
      element.removeAttribute('class');
      return;
    }
    if (name === 'style') {
      element.removeAttribute('style');
      return;
    }
    if (name in element) {
      const current = element[name];
      if (typeof current === 'boolean') element[name] = false;
      else if (name === 'value') element[name] = '';
    }
    element.removeAttribute(name);
  }

  /**
   * @param {Element} element
   * @param {string} eventName
   * @param {EventListener | null} handler
   */
  function updateEvent(element, eventName, handler) {
    let events = renderedEvents.get(element);
    if (!events) {
      events = new Map();
      renderedEvents.set(element, events);
    }
    let registration = events.get(eventName);
    if (!registration) {
      registration = { handler };
      events.set(eventName, registration);
      element.addEventListener(eventName, event => {
        if (registration.handler) registration.handler(event);
      });
    } else {
      registration.handler = handler;
    }
  }

  /**
   * @param {Element} element
   * @param {string} name
   * @param {*} value
   */
  function setProperty(element, name, value) {
    if (value === null || value === undefined) {
      removeProperty(element, name);
      return;
    }
    if (name.startsWith('on')) {
      updateEvent(element, name.slice(2).toLowerCase(), value);
      return;
    }
    if (name === 'className') {
      if (element.className !== value) element.className = value;
      return;
    }
    if (name === 'style') {
      if (element.getAttribute('style') !== value) {
        element.setAttribute('style', String(value));
      }
      return;
    }
    if (name in element && !name.startsWith('aria-')) {
      if (!Object.is(element[name], value)) element[name] = value;
      return;
    }
    const attributeValue = String(value);
    if (element.getAttribute(name) !== attributeValue) {
      element.setAttribute(name, attributeValue);
    }
  }

  /**
   * Update an existing element, changing only properties whose derived values
   * differ from the previous render.
   *
   * @param {Element | string} target
   * @param {Object<string, *>} properties
   * @returns {Element}
   */
  function hu(target, properties = {}) {
    const element = typeof target === 'string' ? byId(target) : target;
    const previous = renderedProperties.get(element) ?? {};
    for (const name of Object.keys(previous)) {
      if (!(name in properties)) removeProperty(element, name);
    }
    for (const [name, value] of Object.entries(properties)) {
      if (name === 'value' || !Object.is(previous[name], value)) {
        setProperty(element, name, value);
      }
    }
    renderedProperties.set(element, { ...properties });
    return element;
  }

  /** @param {string} type @returns {Element} */
  function claimElement(type) {
    const frame = cursor();
    const current = frame.parent.childNodes[frame.index] ?? null;
    let element;
    if (current instanceof Element && current.localName === type) {
      element = current;
    } else {
      element = document.createElement(type);
      if (current) frame.parent.replaceChild(element, current);
      else frame.parent.appendChild(element);
    }
    frame.index += 1;
    return element;
  }

  /** @param {string | number} value */
  function drawText(value) {
    const frame = cursor();
    const current = frame.parent.childNodes[frame.index] ?? null;
    let textNode;
    if (current instanceof Text) {
      textNode = current;
    } else {
      textNode = document.createTextNode('');
      if (current) frame.parent.replaceChild(textNode, current);
      else frame.parent.appendChild(textNode);
    }
    const text = String(value);
    if (textNode.data !== text) textNode.data = text;
    frame.index += 1;
  }

  /** @param {*} child */
  function drawChild(child) {
    if (child === null || child === undefined || typeof child === 'boolean') {
      return;
    }
    if (Array.isArray(child)) {
      child.forEach(drawChild);
      return;
    }
    if (typeof child === 'function') {
      child();
      return;
    }
    drawText(child);
  }

  /** @param {Element} parent @param {*[]} children */
  function drawChildren(parent, children) {
    const previousCursor = renderCursor;
    renderCursor = { parent, index: 0 };
    children.forEach(drawChild);
    while (parent.childNodes.length > renderCursor.index) {
      parent.lastChild.remove();
    }
    renderCursor = previousCursor;
  }

  /**
   * Create or reuse one real DOM element at the current render position.
   * Function components draw directly into that same position.
   *
   * @param {string | Function} type
   * @param {Object<string, *>} [properties]
   * @param {...*} children
   * @returns {Element | null}
   */
  function h(type, properties = {}, ...children) {
    if (typeof type === 'function') {
      type({ ...properties, children });
      return null;
    }
    const element = hu(claimElement(type), properties);
    drawChildren(element, children);
    return element;
  }

  /** @param {HTMLElement | null} element */
  function sessionFocusToken(element) {
    const row = element?.closest?.('[data-session-index]');
    const target = element?.getAttribute?.('data-focus-target');
    return row && target
      ? { index: row.getAttribute('data-session-index'), target }
      : null;
  }

  /** @param {Element} parent @param {{ index: string, target: string } | null} token */
  function sessionFocusTarget(parent, token) {
    if (!token) return null;
    const row = [...parent.querySelectorAll('[data-session-index]')]
      .find(candidate => candidate.getAttribute('data-session-index') ===
        token.index);
    if (row?.getAttribute('data-focus-target') === token.target) return row;
    return row?.querySelector(`[data-focus-target="${token.target}"]`) ?? null;
  }

  /** @param {Function} children */
  function redrawChildren(children) {
    const frame = cursor();
    const active = document.activeElement instanceof HTMLElement
      ? document.activeElement
      : null;
    const activeToken = frame.parent.contains(active)
      ? sessionFocusToken(active)
      : null;
    const returnToken = sessionFocusToken(runtime.modalReturnFocus);
    const openSessions = [...frame.parent.querySelectorAll(
      '[data-session-index]'
    )].filter(row => row.querySelector('details[open]'))
      .map(row => row.getAttribute('data-session-index'));
    frame.parent.replaceChildren();
    frame.index = 0;
    children();
    for (const index of openSessions) {
      const row = [...frame.parent.querySelectorAll('[data-session-index]')]
        .find(candidate => candidate.getAttribute('data-session-index') ===
          index);
      const details = row?.querySelector('details');
      if (details) details.open = true;
    }
    const restoredActive = sessionFocusTarget(frame.parent, activeToken);
    if (restoredActive instanceof HTMLElement) restoredActive.focus();
    const restoredReturn = sessionFocusTarget(frame.parent, returnToken);
    if (restoredReturn instanceof HTMLElement) {
      runtime.modalReturnFocus = restoredReturn;
    }
  }

  /** @param {HTMLElement} root @param {Function} component */
  function draw(root, component) {
    const previousCursor = renderCursor;
    renderCursor = { parent: root, index: 0 };
    component();
    while (root.childNodes.length > renderCursor.index) {
      root.lastChild.remove();
    }
    renderCursor = previousCursor;
  }

  /**
   * @param {...(string | false | null | undefined)} values
   * @returns {string}
   */
  function classes(...values) {
    return values.filter(Boolean).join(' ');
  }

  /** @param {string} kind @param {Object<string, *>} [detail] */
  function post(kind, detail = {}) {
    if (!runtime.webview) return;
    runtime.webview.postMessage({
      version: protocolVersion,
      kind,
      pageGeneration: runtime.pageGeneration,
      ...detail
    });
  }

  /** @param {{ glyph: string }} properties */
  function Icon({ glyph }) {
    h('span', { className: 'icon' }, glyph);
  }

  /**
   * @param {{
   *   id: string,
   *   label: string,
   *   active: boolean,
   *   disabled?: boolean,
   *   progress?: number | null,
   *   done?: boolean,
   *   onClick?: EventListener
   * }} properties
   */
  function WorkflowTab({
    id,
    label,
    active,
    disabled = false,
    progress = null,
    done = false,
    onClick = null
  }) {
    h('button', {
      id,
      className: classes(
        'button tab flex-1/3',
        progress !== null && 'progress',
        done && 'progress-done'
      ),
      'aria-current': active,
      type: 'button',
      disabled,
      style: progress === null ? null : `--progress: ${progress}%`,
      onClick
    }, label);
  }

  /** @param {{ snapshot: PanoramaSnapshot | null }} properties */
  function WorkflowNavigation({ snapshot }) {
    const stage = snapshot?.stage ?? 'input';
    h('nav', { className: 'flex gap-4', 'aria-label': 'Workflow' }, () => {
      h(WorkflowTab, {
        id: 'tab-input',
        label: 'Input',
        active: stage === 'input',
        onClick: () => post('navigate', { target: 'input' })
      });
      h(WorkflowTab, {
        id: 'tab-preview',
        label: 'Preview',
        active: stage === 'preview',
        progress: snapshot?.busy && !snapshot?.rendering
          ? snapshot.previewProgress
          : null,
        done: snapshot?.previewReady ?? false,
        onClick: () => post('navigate', { target: 'preview' })
      });
      h(WorkflowTab, {
        id: 'tab-output',
        label: 'Output',
        active: stage === 'output',
        disabled: !(snapshot?.previewReady ?? false),
        progress: snapshot?.rendering ? snapshot.outputProgress : null,
        done: !(snapshot?.rendering ?? false) &&
          (snapshot?.outputComplete ?? false),
        onClick: () => post('navigate', { target: 'output' })
      });
      h('button', {
        id: 'settings',
        className: 'button',
        type: 'button',
        'aria-label': 'App settings',
        title: 'App settings',
        onClick: () => post('open-settings')
      }, () => h(Icon, { glyph: '\uE713' }));
    });
  }

  /**
   * @param {{
   *   id: string,
   *   label: string,
   *   value: string,
   *   target: string,
   *   refresh?: boolean,
   *   disabled?: boolean
   * }} properties
   */
  function DirectoryField({
    id,
    label,
    value,
    target,
    refresh = false,
    disabled = false
  }) {
    h('label', { className: 'flex flex-col gap-2' }, () => {
      h('span', {}, label);
      h('span', { className: 'flex gap-4' }, () => {
        h('input', {
          id,
          className: 'input-text flex-1 min-w-0',
          'aria-label': label.replace(/:$/, ''),
          value,
          disabled,
          onChange: event => post('set-directory', {
            target,
            value: event.currentTarget.value
          })
        });
        h('button', {
          id: `browse-${target}`,
          className: 'button',
          type: 'button',
          'aria-label': `Browse ${label.toLowerCase().replace(/:$/, '')}`,
          title: 'Browse',
          disabled,
          onClick: () => post('browse-directory', { target })
        }, () => h(Icon, { glyph: '\uED25' }));
        if (refresh) {
          h('button', {
            id: 'refresh',
            className: 'button',
            type: 'button',
            'aria-label': 'Refresh sessions',
            title: 'Refresh sessions',
            disabled,
            onClick: () => post('refresh')
          }, () => h(Icon, { glyph: '\uEDAB' }));
        }
      });
    });
  }

  /** @param {{ label: string, focusTarget: string, disabled: boolean, onClick: EventListener }} properties */
  function ActionsMenuItem({ label, focusTarget, disabled, onClick }) {
    h('button', {
      type: 'button',
      'data-focus-target': focusTarget,
      disabled,
      onClick
    }, label);
  }

  /**
   * @param {{
   *   session: PanoramaSessionSnapshot,
   *   index: number,
   *   selected: boolean,
   *   disabled: boolean
   * }} properties
   */
  function SessionRow({ session, index, selected, disabled }) {
    const statusClass = session.status === 'invalid'
      ? 'text-red-500'
      : session.status === 'incomplete'
        ? 'text-amber-400'
        : session.status === 'stitched'
          ? 'text-green-500'
          : null;
    h('tr', {
      className: 'even:bg-gray-800',
      tabIndex: disabled ? -1 : 0,
      'data-session-index': index,
      'data-focus-target': 'row',
      'aria-selected': selected,
      'aria-disabled': disabled,
      onClick: event => {
        if (!disabled && !event.target.closest('button, details')) {
          post('select-session', { index });
        }
      },
      onKeydown: event => {
        if (disabled || event.target !== event.currentTarget) return;
        if (event.key === 'Enter' || event.key === ' ') {
          event.preventDefault();
          post('select-session', { index });
        }
      }
    }, () => {
      h('td', { className: 'w-12 p-2' }, () => {
        h('input', {
          className: 'input-checkbox',
          type: 'checkbox',
          checked: selected,
          disabled,
          'aria-label': `Select ${session.name}`,
          'data-focus-target': 'selection',
          onClick: event => event.preventDefault()
        });
      });
      h('td', {
        className: classes('p-2', statusClass),
        title: session.detail || null
      }, session.name);
      h('td', { className: 'p-2' }, session.poses);
      h('td', { className: 'p-2' }, session.tag || '\u00a0');
      h('td', { className: 'relative p-2' }, () => {
        h('details', { className: 'relative' }, () => {
          h('summary', {
            className: 'button sm',
            style: 'display: inline-flex',
            'data-focus-target': 'actions',
            'aria-haspopup': 'menu'
          }, () => {
            h('span', {}, 'Actions');
            h('span', {
              className: 'h-6 w-[1px] border-r border-gray-400'
            });
            h(Icon, { glyph: '\uE70D' });
          });
          h('div', {
            className: 'actions-menu absolute right-2 z-10 min-w-[150px] rounded-md border border-gray-500 bg-gray-900 p-1',
            role: 'menu'
          }, () => {
            h(ActionsMenuItem, {
              label: 'Copy coordinates',
              focusTarget: 'copy-coordinates',
              disabled: disabled || !session.hasCoordinates,
              onClick: () => post('copy-session-coordinates', { index })
            });
            h(ActionsMenuItem, {
              label: 'Edit tag...',
              focusTarget: 'edit-tag',
              disabled,
              onClick: () => post('edit-tag', { index })
            });
            h(ActionsMenuItem, {
              label: 'Delete session...',
              focusTarget: 'delete-session',
              disabled,
              onClick: () => post('delete-session', { index })
            });
          });
        });
      });
    });
  }

  /** @param {{ snapshot: PanoramaSnapshot | null }} properties */
  function SessionTable({ snapshot }) {
    h('div', {
      className: 'min-h-72 flex-1 overflow-auto border border-gray-500 bg-gray-900'
    }, () => {
      h('table', { className: 'w-full', 'aria-label': 'Panorama sessions' }, () => {
        h('thead', { className: 'sticky top-0' }, () => {
          h('tr', { className: 'bg-gray-600' }, () => {
            h('th', { className: 'w-12 p-2', 'aria-label': 'Selected' });
            h('th', { className: 'p-2 text-left' }, 'Session');
            h('th', { className: 'p-2 text-left' }, '#');
            h('th', { className: 'p-2 text-left' }, 'Tag');
            h('th', { className: 'p-2 text-left' });
          });
        });
        h('tbody', { id: 'sessions' }, () => {
          redrawChildren(() => {
            snapshot?.sessions.forEach((session, index) => {
              h(SessionRow, {
                session,
                index,
                selected: index === snapshot.selectedIndex,
                disabled: snapshot.busy
              });
            });
          });
        });
      });
    });
  }

  /** @param {{ snapshot: PanoramaSnapshot | null }} properties */
  function InputView({ snapshot }) {
    h('section', {
      id: 'input-view',
      className: 'flex flex-1 flex-col gap-4 rounded-md border border-gray-500 p-4',
      'aria-label': 'Input settings',
      hidden: snapshot !== null && snapshot.stage !== 'input'
    }, () => {
      h(DirectoryField, {
        id: 'game-directory',
        label: 'Game directory:',
        value: snapshot?.gameDirectory ?? '',
        target: 'game',
        refresh: true,
        disabled: snapshot?.busy ?? false
      });
      h(SessionTable, { snapshot });
      h(DirectoryField, {
        id: 'screenshots-directory',
        label: 'Screenshots directory:',
        value: snapshot?.screenshotsDirectory ?? '',
        target: 'screenshots',
        disabled: snapshot?.busy ?? false
      });
    });
  }

  /** @param {{ snapshot: PanoramaSnapshot | null }} properties */
  function PreviewView({ snapshot }) {
    const preview = snapshot?.stage === 'preview';
    const ready = snapshot?.previewReady ?? false;
    h('section', {
      id: 'preview-view',
      className: 'flex flex-1 flex-col gap-4 rounded-md border border-gray-500 p-4',
      'aria-label': 'Panorama preview',
      hidden: !preview
    }, () => {
      h('div', {
        id: 'preview-frame'
      }, () => {
        h('div', {
          id: 'preview-placeholder',
          className: 'flex aspect-[2/1] w-full items-center justify-center overflow-hidden'
        }, () => {
          h('span', { id: 'preview-message' },
            ready ? '' : snapshot?.previewMessage ?? 'Loading...');
        });
      });
      h('div', { className: 'flex items-center gap-4' }, () => {
        h('span', {
          id: 'exposure-adjusted',
          className: 'flex-1 text-gray-400 italic'
        }, snapshot?.exposureAdjusted ? 'Exposure has been adjusted' : '');
        h('button', {
          id: 'adjust-exposure',
          className: 'button',
          type: 'button',
          onClick: () => post('open-exposure')
        }, 'Adjust exposure ', () => h(Icon, {
          glyph: snapshot?.exposureOpen ? '\uE76B' : '\uE76C'
        }));
      });
    });
  }

  /**
   * @param {{
   *   id: string,
   *   value: string,
   *   maximum: number,
   *   widthClass: string,
   *   suffix: string,
   *   target: string,
   *   disabled: boolean
   * }} properties
   */
  function OutputRange({
    id,
    value,
    maximum,
    widthClass,
    suffix,
    target,
    disabled
  }) {
    const update = event => {
      const raw = event.currentTarget.value;
      const numeric = Number(raw);
      const value = target === 'width' && raw !== '' && Number.isFinite(numeric)
        ? String(Math.min(maximum, Math.max(1, numeric)))
        : raw;
      post('set-output-value', { target, value });
    };
    h('div', { className: 'flex w-100 items-center gap-2' }, () => {
      h('input', {
        id: `${id}-slider`,
        className: 'input-range flex-1',
        type: 'range',
        min: 1,
        max: maximum,
        value,
        disabled,
        onInput: update
      });
      h('div', {}, () => {
        h('input', {
          id,
          className: `input-text sm ${widthClass}`,
          type: 'text',
          inputMode: 'numeric',
          maxLength: maximum === 100 ? 3 : 5,
          value,
          disabled,
          onInput: update
        });
        h('span', {}, ` ${suffix}`);
      });
    });
  }

  /** @param {{ snapshot: PanoramaSnapshot | null }} properties */
  function OutputView({ snapshot }) {
    const output = snapshot?.stage === 'output';
    const pixels = snapshot?.resolutionPixels ?? false;
    const format = snapshot?.outputFormat ?? 'jpeg';
    const busy = snapshot?.busy ?? false;
    h('section', {
      id: 'output-view',
      className: 'flex flex-1 flex-col gap-4 rounded-md border border-gray-500 p-4',
      'aria-label': 'Output settings',
      hidden: !output
    }, () => {
      h(DirectoryField, {
        id: 'output-directory',
        label: 'Output directory:',
        value: snapshot?.outputDirectory ?? '',
        target: 'output',
        disabled: busy
      });
      h('label', { className: 'flex flex-col gap-2' }, () => {
        h('span', {}, 'Filename:');
        h('input', {
          id: 'output-name',
          className: 'input-text flex-1',
          type: 'text',
          value: snapshot?.outputName ?? '',
          disabled: busy,
          onInput: event => post('set-output-value', {
            target: 'name',
            value: event.currentTarget.value
          })
        });
      });
      h('div', { className: 'flex flex-col gap-2' }, () => {
        h('div', { className: 'flex items-center gap-4' }, () => {
          h('div', { className: 'flex items-center gap-2' }, () => {
            h('button', {
              id: 'resolution-mode',
              className: 'button sm',
              type: 'button',
              disabled: busy,
              'aria-label': pixels ? 'Use percentage scaling' : 'Use pixel width',
              title: pixels ? 'Use percentage scaling' : 'Use pixel width',
              onClick: () => post('toggle-resolution-mode')
            }, () => h(Icon, { glyph: '\uF1CB' }));
            h('span', {}, pixels ? 'Width (px):' : 'Scaling (%):');
          });
          h(OutputRange, {
            id: pixels ? 'output-width' : 'output-scale',
            value: pixels
              ? snapshot?.outputWidth ?? ''
              : snapshot?.resolutionPercent ?? '100',
            maximum: pixels
              ? Math.max(1, snapshot?.outputMaximumWidth ?? 1)
              : 100,
            widthClass: pixels ? 'w-16' : 'w-12',
            suffix: pixels ? 'px' : '%',
            target: pixels ? 'width' : 'scale',
            disabled: busy
          });
        });
        h('div', { className: 'text-gray-400' },
          snapshot?.outputSummary ?? '');
      });
      h('div', { className: 'flex flex-col gap-2' }, () => {
        h('div', { className: 'flex items-center gap-4' }, () => {
          h('span', {}, 'Format:');
          [['jpeg', 'JPEG (SDR)'], ['png', 'PNG (SDR)'], ['exr', 'EXR (HDR)']]
            .forEach(([value, label]) => {
              h('label', { className: 'flex items-center gap-2' }, () => {
                h('input', {
                  className: 'input-radio',
                  type: 'radio',
                  name: 'output-format',
                  value,
                  checked: format === value,
                  disabled: busy,
                  onChange: event => {
                    if (event.currentTarget.checked) {
                      post('set-output-value', { target: 'format', value });
                    }
                  }
                });
                h('span', {}, label);
              });
            });
        });
        if (format === 'jpeg') {
          h('div', { className: 'flex items-center gap-4' }, () => {
            h('span', {}, 'Quality (%):');
            h(OutputRange, {
              id: 'jpeg-quality',
              value: snapshot?.jpegQuality ?? '95',
              maximum: 100,
              widthClass: 'w-12',
              suffix: '%',
              target: 'quality',
              disabled: busy
            });
          });
        }
      });
    });
  }

  /** @param {{ snapshot: PanoramaSnapshot | null }} properties */
  function Footer({ snapshot }) {
    const preview = snapshot?.stage === 'preview';
    const output = snapshot?.stage === 'output';
    const busy = snapshot?.busy ?? false;
    const ready = snapshot?.previewReady ?? false;
    h('footer', { className: 'flex items-center justify-between gap-4' }, () => {
      h('span', { className: 'flex min-w-0 items-center gap-4' }, () => {
        h('span', { id: 'status', role: 'status' },
          snapshot?.status ?? 'Loading application state...');
        h('button', {
          id: 'abort',
          className: 'button sm shrink-0',
          type: 'button',
          hidden: !busy,
          onClick: () => post('abort')
        }, 'Abort');
      });
      h('span', { className: 'flex items-center gap-4' }, () => {
        if (output) {
          h('button', {
            id: 'render-thumbnail',
            className: 'button primary shrink-0',
            type: 'button',
            disabled: busy || !(snapshot?.renderEnabled ?? false),
            onClick: () => post('render-with-thumbnail')
          }, 'Render with thumbnail');
          h('button', {
            id: 'render',
            className: 'button primary shrink-0',
            type: 'button',
            disabled: busy || !(snapshot?.renderEnabled ?? false),
            onClick: () => post('render')
          }, 'Render');
        } else {
          h('button', {
            id: 'options',
            className: 'button shrink-0',
            type: 'button',
            disabled: busy,
            onClick: () => post('open-options')
          }, () => h(Icon, { glyph: '\uE70F' }), ' Options');
          h('button', {
            id: 'primary',
            className: 'button primary shrink-0',
            type: 'button',
            disabled: preview ? !ready : !(snapshot?.previewEnabled ?? false),
            onClick: () => post(preview ? 'finalize' : 'start-preview')
          }, preview ? 'Finalize' : 'Preview');
        }
      });
    });
  }

  /** @param {{ visible: boolean }} properties */
  function BridgeError({ visible }) {
    h('section', {
      id: 'bridge-error',
      className: 'bridge-error text-gray-300',
      hidden: !visible
    }, () => {
      h('strong', {}, 'The application page lost its native connection.');
      h('span', {},
        'Reload the page to request a fresh, authoritative state snapshot.');
      h('button', {
        id: 'reload',
        className: 'button primary',
        type: 'button',
        onClick: () => location.reload()
      }, 'Reload');
    });
  }

  /** @param {PanoramaModalSnapshot} modal */
  function dismissModal(modal) {
    if (modal.dismissible) {
      post('dismiss-modal', { modalGeneration: modal.generation });
    }
  }

  /** @returns {HTMLElement[]} */
  function modalFocusTargets() {
    const dialog = byId('modal-dialog');
    return [...dialog.querySelectorAll(
      'button:not(:disabled), input:not(:disabled), select:not(:disabled), ' +
      'textarea:not(:disabled), [href], [tabindex]:not([tabindex="-1"])'
    )].filter(element => element instanceof HTMLElement && !element.hidden);
  }

  /** @param {PanoramaModalSnapshot} modal */
  function EditTagModal({ modal }) {
    h('label', { className: 'flex flex-col gap-2' }, () => {
      h('span', {}, 'Tag');
      h('input', {
        id: 'modal-tag',
        className: 'input-text w-full',
        type: 'text',
        maxLength: 128,
        value: modal.payload.value,
        disabled: modal.payload.readOnly,
        onInput: event => post('set-modal-value', {
          modalGeneration: modal.generation,
          value: event.currentTarget.value
        }),
        onKeydown: event => {
          if (event.key === 'Enter' && modal.payload.canSubmit) {
            event.preventDefault();
            post('submit-modal', { modalGeneration: modal.generation });
          }
        }
      });
    });
    h('span', {
      id: 'modal-character-count',
      className: 'text-gray-400'
    }, `${modal.payload.charactersRemaining} characters remaining`);
    h('p', {
      id: 'modal-error',
      className: 'text-red-500',
      role: 'alert',
      hidden: !modal.payload.error
    }, modal.payload.error);
  }

  /** @param {PanoramaModalSnapshot} modal */
  function InputOptionsModal({ modal }) {
    h('label', { className: 'flex items-center gap-2' }, () => {
      h('input', {
        id: 'modal-allow-incomplete',
        className: 'input-checkbox',
        type: 'checkbox',
        checked: modal.payload.checked,
        disabled: modal.payload.readOnly,
        onChange: event => post('set-modal-toggle', {
          modalGeneration: modal.generation,
          enabled: event.currentTarget.checked
        })
      });
      h('span', {}, 'Allow incomplete session');
    });
    h('p', {
      id: 'modal-error',
      className: 'text-red-500',
      role: 'alert',
      hidden: !modal.payload.error
    }, modal.payload.error);
  }

  /** @param {PanoramaModalSnapshot} modal */
  function PreviewOptionsModal({ modal }) {
    h('fieldset', { className: 'flex flex-row items-center gap-4' }, () => {
      h('legend', {}, 'Blending:');
      [['hard', 'Hard'], ['feather', 'Feather']].forEach(([value, label]) => {
        h('label', { className: 'flex items-center gap-2' }, () => {
          h('input', {
            className: 'input-radio',
            type: 'radio',
            name: 'modal-blend',
            value,
            checked: modal.payload.value === value,
            disabled: modal.payload.readOnly,
            onChange: event => {
              if (event.currentTarget.checked) {
                post('set-modal-value', {
                  modalGeneration: modal.generation,
                  value
                });
              }
            }
          });
          h('span', {}, label);
        });
      });
    });
    h('label', { className: 'flex items-center gap-2' }, () => {
      h('input', {
        id: 'modal-auto-contrast',
        className: 'input-checkbox',
        type: 'checkbox',
        checked: modal.payload.checked,
        disabled: modal.payload.readOnly,
        onChange: event => post('set-modal-toggle', {
          modalGeneration: modal.generation,
          enabled: event.currentTarget.checked
        })
      });
      h('span', {}, 'Auto contrast (SDR only)');
    });
    h('p', {
      id: 'modal-error',
      className: 'text-red-500',
      role: 'alert',
      hidden: !modal.payload.error
    }, modal.payload.error);
  }

  /** @param {PanoramaModalSnapshot} modal */
  function AppSettingsModal({ modal }) {
    h('label', { className: 'flex flex-col gap-2' }, () => {
      h('span', {}, 'D3D12 allocation (MiB):');
      h('input', {
        id: 'modal-gpu-memory',
        className: 'input-text w-full',
        type: 'text',
        inputMode: 'numeric',
        value: modal.payload.value,
        disabled: modal.payload.readOnly,
        onInput: event => post('set-modal-value', {
          modalGeneration: modal.generation,
          value: event.currentTarget.value
        }),
        onKeydown: event => {
          if (event.key === 'Enter' && modal.payload.canSubmit) {
            event.preventDefault();
            post('submit-modal', { modalGeneration: modal.generation });
          }
        }
      });
    });
    h('label', { className: 'flex items-center gap-2' }, () => {
      h('input', {
        id: 'modal-debug-coverage',
        className: 'input-checkbox',
        type: 'checkbox',
        checked: modal.payload.checked,
        disabled: modal.payload.readOnly,
        onChange: event => post('set-modal-toggle', {
          modalGeneration: modal.generation,
          enabled: event.currentTarget.checked
        })
      });
      h('span', {}, 'Write debug coverage image');
    });
    h('p', {
      id: 'modal-error',
      className: 'text-red-500',
      role: 'alert',
      hidden: !modal.payload.error
    }, modal.payload.error);
  }

  /** @param {PanoramaModalSnapshot} modal */
  function DestructiveConfirmationModal({ modal }) {
    if (modal.kind === 'delete-session') {
      h('label', { className: 'flex items-center gap-2' }, () => {
        h('input', {
          id: 'modal-delete-images',
          className: 'input-checkbox',
          type: 'checkbox',
          checked: modal.payload.checked,
          disabled: modal.payload.readOnly,
          onChange: event => post('set-modal-toggle', {
            modalGeneration: modal.generation,
            enabled: event.currentTarget.checked
          })
        });
        h('span', {}, 'Also delete captured screenshots');
      });
    }
    h('p', {
      id: 'modal-error',
      className: 'text-red-500',
      role: 'alert',
      hidden: !modal.payload.error
    }, modal.payload.error);
  }

  /** @param {PanoramaModalSnapshot | null} modal */
  function ModalHost({ modal }) {
    const open = modal !== null;
    const saves = modal?.kind === 'edit-tag' ||
      modal?.kind === 'input-options' || modal?.kind === 'preview-options' ||
      modal?.kind === 'app-settings' || modal?.kind === 'delete-session' ||
      modal?.kind === 'overwrite-output';
    const action = modal?.kind === 'delete-session'
      ? 'Delete'
      : modal?.kind === 'overwrite-output' ? 'Replace' : 'Save';
    const title = modal?.payload.title ?? '';
    const description = modal?.payload.description ?? '';
    const deleteDescription = modal?.kind === 'delete-session'
      ? description.split('\n')
      : [];
    const describedBy = modal?.kind === 'delete-session'
      ? 'modal-description modal-file-list'
      : (modal?.kind === 'overwrite-output' || modal?.kind === 'notice') && description
        ? 'modal-description'
        : null;
    h('div', {
      id: 'modal-layer',
      className: 'modal-layer',
      hidden: !open,
      onClick: event => {
        if (event.target === event.currentTarget && modal) dismissModal(modal);
      }
    }, () => {
      h('section', {
        id: 'modal-dialog',
        className: 'modal-dialog flex flex-col gap-4 rounded-md border border-gray-500 bg-gray-950 p-4 text-gray-300',
        role: 'dialog',
        'aria-modal': 'true',
        'aria-labelledby': 'modal-title',
        'aria-describedby': describedBy,
        'data-modal-kind': modal?.kind ?? '',
        tabIndex: -1
      }, () => {
        h('h2', { id: 'modal-title', className: 'modal-title' }, title);
        h('div', { id: 'modal-content', className: 'flex flex-col gap-2' }, () => {
          if (modal?.kind === 'delete-session') {
            h('p', { id: 'modal-description' }, deleteDescription[0] ?? '');
            h('ul', { id: 'modal-file-list' }, () => {
              deleteDescription.slice(1).forEach(path => h('li', {}, path));
            });
          } else if (modal?.kind === 'overwrite-output' || modal?.kind === 'notice') {
            h('p', { id: 'modal-description' }, description);
          }
          if (modal?.kind === 'edit-tag') h(EditTagModal, { modal });
          if (modal?.kind === 'input-options') h(InputOptionsModal, { modal });
          if (modal?.kind === 'preview-options') h(PreviewOptionsModal, { modal });
          if (modal?.kind === 'app-settings') h(AppSettingsModal, { modal });
          if (modal?.kind === 'delete-session' ||
              modal?.kind === 'overwrite-output') {
            h(DestructiveConfirmationModal, { modal });
          }
        });
        h('div', { className: 'flex justify-end gap-4' }, () => {
          h('button', {
            id: 'modal-close',
            className: classes('button', !saves && 'primary'),
            type: 'button',
            hidden: !(modal?.dismissible ?? false),
            onClick: () => {
              if (modal) dismissModal(modal);
            }
          }, saves ? 'Cancel' : 'Close');
          if (saves) {
            h('button', {
              id: 'modal-save',
              className: 'button primary',
              type: 'button',
              disabled: !modal.payload.canSubmit,
              onClick: () => post('submit-modal', {
                modalGeneration: modal.generation
              })
            }, action);
          }
        });
      });
    });
  }

  /**
   * @param {{
   *   snapshot: PanoramaSnapshot | null,
   *   bridgeFailed: boolean
   * }} properties
   */
  function App({ snapshot, bridgeFailed }) {
    const modal = snapshot?.modal ?? null;
    h('main', {
      id: 'app',
      className: classes(
        'flex min-h-screen w-full flex-col gap-4 bg-gray-950 p-4 text-gray-300',
        snapshot?.maximized && 'maximized'
      ),
      inert: modal !== null
    }, () => {
      h(WorkflowNavigation, { snapshot });
      h(InputView, { snapshot });
      h(PreviewView, { snapshot });
      h(OutputView, { snapshot });
      h(Footer, { snapshot });
    });
    h(ModalHost, { modal });
    h(BridgeError, { visible: bridgeFailed });
  }

  function drawApp() {
    draw(byId('ui-root'), () => h(App, {
      snapshot: runtime.snapshot,
      bridgeFailed: runtime.bridgeFailed
    }));
  }

  function updateDocumentOverflow() {
    const height = runtime.naturalContentHeight;
    const contentFits = height !== null &&
      height <= document.documentElement.clientHeight + 1;
    document.body.style.overflowY = contentFits ? 'hidden' : 'auto';
  }

  function reportContentSize() {
    runtime.contentSizeFrame = 0;
    const app = byId('app');
    const visibleView = runtime.snapshot?.stage === 'preview'
      ? byId('preview-view')
      : runtime.snapshot?.stage === 'output'
        ? byId('output-view')
        : byId('input-view');
    const previousMinHeight = app.style.minHeight;
    const previousFlex = visibleView.style.flex;
    const previousOverflow = document.body.style.overflowY;
    document.body.style.overflowY = 'hidden';
    app.style.minHeight = '0px';
    visibleView.style.flex = 'none';
    const height = Math.ceil(app.getBoundingClientRect().height);
    app.style.minHeight = previousMinHeight;
    visibleView.style.flex = previousFlex;
    if (!Number.isFinite(height) || height <= 0) {
      document.body.style.overflowY = previousOverflow;
      return;
    }
    runtime.naturalContentHeight = height;
    updateDocumentOverflow();
    const layoutGeneration = runtime.snapshot?.layoutGeneration ?? 0;
    if (runtime.lastContentHeight === height &&
        runtime.lastContentLayoutGeneration === layoutGeneration) return;
    runtime.lastContentHeight = height;
    runtime.lastContentLayoutGeneration = layoutGeneration;
    post('content-size', {
      layoutGeneration,
      height
    });
  }

  function queueContentSize() {
    if (!runtime.contentSizeFrame) {
      runtime.contentSizeFrame = requestAnimationFrame(reportContentSize);
    }
  }

  function reportPreviewGeometry() {
    runtime.geometryFrame = 0;
    const placeholder = byId('preview-placeholder');
    const rect = placeholder.getBoundingClientRect();
    const snapshot = runtime.snapshot;
    const fullyVisible = snapshot?.stage === 'preview' &&
      snapshot.previewReady &&
      snapshot.modal === null &&
      !byId('preview-view').hidden &&
      rect.width > 0 && rect.height > 0 &&
      rect.left >= 0 && rect.top >= 0 &&
      rect.right <= document.documentElement.clientWidth &&
      rect.bottom <= document.documentElement.clientHeight;
    const geometry = {
      layoutGeneration: snapshot?.layoutGeneration ?? 0,
      x: rect.x,
      y: rect.y,
      width: rect.width,
      height: rect.height,
      deviceScale: window.devicePixelRatio,
      visible: fullyVisible
    };
    const signature = JSON.stringify(geometry);
    if (runtime.lastPreviewGeometry === signature) return;
    runtime.lastPreviewGeometry = signature;
    post('preview-geometry', geometry);
  }

  /** @param {PanoramaModalSnapshot | null} previous @param {PanoramaModalSnapshot | null} modal */
  function syncModalFocus(previous, modal) {
    if (modal && (!previous || previous.generation !== modal.generation)) {
      const targets = modalFocusTargets();
      (targets[0] ?? byId('modal-dialog')).focus();
      return;
    }
    if (!modal && previous) {
      const target = runtime.modalReturnFocus;
      runtime.modalReturnFocus = null;
      if (target?.isConnected) target.focus();
    }
  }

  function queuePreviewGeometry() {
    if (!runtime.geometryFrame) {
      runtime.geometryFrame = requestAnimationFrame(reportPreviewGeometry);
    }
  }

  function handleWindowResize() {
    updateDocumentOverflow();
    queueContentSize();
    queuePreviewGeometry();
    if (runtime.deviceScale !== window.devicePixelRatio) {
      runtime.deviceScale = window.devicePixelRatio;
      runtime.lastContentHeight = null;
      runtime.lastContentLayoutGeneration = null;
    }
  }

  /** @param {PanoramaSnapshot} snapshot */
  function render(snapshot) {
    if (snapshot.version !== protocolVersion || snapshot.kind !== 'snapshot') {
      runtime.bridgeFailed = true;
      drawApp();
      return;
    }
    const previousModal = runtime.snapshot?.modal ?? null;
    if (snapshot.modal && !previousModal) {
      const active = document.activeElement;
      runtime.modalReturnFocus = active instanceof HTMLElement ? active : null;
    }
    runtime.snapshot = snapshot;
    runtime.pageGeneration = snapshot.pageGeneration;
    runtime.bridgeFailed = false;
    drawApp();
    syncModalFocus(previousModal, snapshot.modal);
    queueContentSize();
    queuePreviewGeometry();
  }

  window.addEventListener('error', () => {
    if (!runtime.bridgeFailed) {
      runtime.bridgeFailed = true;
      drawApp();
    }
  });

  document.addEventListener('focusin', event => {
    if (runtime.bridgeFailed || !runtime.snapshot?.modal ||
        byId('modal-dialog').contains(event.target)) return;
    const targets = modalFocusTargets();
    (targets[0] ?? byId('modal-dialog')).focus();
  });

  document.addEventListener('toggle', event => {
    const opened = event.target;
    if (!(opened instanceof HTMLDetailsElement) || !opened.open ||
        !opened.closest('#sessions')) return;
    document.querySelectorAll('#sessions details[open]').forEach(details => {
      if (details !== opened) details.open = false;
    });
  }, true);

  document.addEventListener('pointerdown', event => {
    if (event.target instanceof Element &&
        event.target.closest('#sessions details')) return;
    document.querySelectorAll('#sessions details[open]').forEach(details => {
      details.open = false;
    });
  });

  document.addEventListener('keydown', event => {
    const modal = runtime.snapshot?.modal;
    if (runtime.bridgeFailed || !modal) return;
    if (event.key === 'Escape' && modal.dismissible) {
      event.preventDefault();
      dismissModal(modal);
      return;
    }
    if (event.key !== 'Tab') return;
    const targets = modalFocusTargets();
    if (targets.length === 0) {
      event.preventDefault();
      byId('modal-dialog').focus();
      return;
    }
    const first = targets[0];
    const last = targets[targets.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  });

  drawApp();
  new ResizeObserver(queuePreviewGeometry).observe(byId('preview-placeholder'));
  window.addEventListener('resize', handleWindowResize);
  window.addEventListener('scroll', queuePreviewGeometry, true);

  const webview = window.chrome?.webview ?? null;
  if (webview) {
    runtime.webview = webview;
    webview.addEventListener('message', event => render(event.data));
    post('ready');
  }
})();
