/**
 * @typedef {'input' | 'preview'} WorkflowStage
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
 */

/**
 * Complete authoritative state sent from the native application.
 *
 * @typedef {Object} PanoramaSnapshot
 * @property {1} version Native/WebView protocol version.
 * @property {'snapshot'} kind Message discriminator.
 * @property {number} pageGeneration Current HTML document generation.
 * @property {number} layoutGeneration Current preview-layout generation.
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
 * @property {PanoramaSessionSnapshot[]} sessions
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

  /** @param {Function} children */
  function redrawChildren(children) {
    const frame = cursor();
    frame.parent.replaceChildren();
    frame.index = 0;
    children();
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
        progress: snapshot?.busy
          ? snapshot.previewProgress
          : null,
        done: !(snapshot?.busy ?? false) && (snapshot?.previewReady ?? false),
        onClick: () => post('navigate', { target: 'preview' })
      });
      h(WorkflowTab, {
        id: 'tab-output',
        label: 'Output',
        active: false,
        disabled: true
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
          className: 'flex-1 min-w-0 border border-gray-500 bg-gray-900 p-2',
          'aria-label': label.replace(/:$/, ''),
          value,
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

  /** @param {{ label: string, onClick: EventListener }} properties */
  function ActionsMenuItem({ label, onClick }) {
    h('button', { type: 'button', onClick }, label);
  }

  /**
   * @param {{
   *   session: PanoramaSessionSnapshot,
   *   index: number,
   *   selected: boolean
   * }} properties
   */
  function SessionRow({ session, index, selected }) {
    const statusClass = session.status === 'invalid'
      ? 'text-red-500'
      : session.status === 'incomplete'
        ? 'text-amber-400'
        : session.status === 'stitched'
          ? 'text-green-500'
          : null;
    h('tr', {
      className: 'even:bg-gray-800',
      tabIndex: 0,
      'aria-selected': selected,
      onClick: event => {
        if (!event.target.closest('button, details')) {
          post('select-session', { index });
        }
      },
      onKeydown: event => {
        if (event.target !== event.currentTarget) return;
        if (event.key === 'Enter' || event.key === ' ') {
          event.preventDefault();
          post('select-session', { index });
        }
      }
    }, () => {
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
              label: 'Edit tag...',
              onClick: () => post('edit-tag', { index })
            });
            h(ActionsMenuItem, {
              label: 'Delete session...',
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
                selected: index === snapshot.selectedIndex
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
        target: 'screenshots'
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
        id: 'preview-placeholder',
        className: 'flex aspect-[2/1] w-full items-center justify-center overflow-hidden'
      }, () => {
        h('span', { id: 'preview-message' },
          ready ? '' : snapshot?.previewMessage ?? 'Loading...');
      });
      h('div', {
        className: 'flex items-center gap-4',
        hidden: !preview || !ready
      }, () => {
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

  /** @param {{ snapshot: PanoramaSnapshot | null }} properties */
  function Footer({ snapshot }) {
    const preview = snapshot?.stage === 'preview';
    const busy = snapshot?.busy ?? false;
    const ready = snapshot?.previewReady ?? false;
    h('footer', { className: 'flex items-center justify-between gap-4' }, () => {
      h('span', { className: 'flex min-w-0 items-center gap-4' }, () => {
        h('span', { id: 'status', role: 'status' },
          snapshot?.status ?? 'Loading application state...');
        h('button', {
          id: 'abort',
          className: 'button sm',
          type: 'button',
          hidden: !busy,
          onClick: () => post('abort')
        }, 'Abort');
      });
      h('span', { className: 'flex items-center gap-4' }, () => {
        h('button', {
          id: 'options',
          className: 'button',
          type: 'button',
          disabled: busy,
          onClick: () => post('open-options')
        }, () => h(Icon, { glyph: '\uE70F' }), ' Options');
        h('button', {
          id: 'primary',
          className: 'button primary',
          type: 'button',
          disabled: preview ? !ready : !(snapshot?.previewEnabled ?? false),
          onClick: () => post(preview ? 'finalize' : 'start-preview')
        }, preview ? 'Finalize' : 'Preview');
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

  /**
   * @param {{
   *   snapshot: PanoramaSnapshot | null,
   *   bridgeFailed: boolean
   * }} properties
   */
  function App({ snapshot, bridgeFailed }) {
    h('main', {
      id: 'app',
      className: 'flex min-h-screen w-full flex-col gap-4 bg-gray-950 p-4 text-gray-300'
    }, () => {
      h(WorkflowNavigation, { snapshot });
      h(InputView, { snapshot });
      h(PreviewView, { snapshot });
      h(Footer, { snapshot });
    });
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

  function queuePreviewGeometry() {
    if (!runtime.geometryFrame) {
      runtime.geometryFrame = requestAnimationFrame(reportPreviewGeometry);
    }
  }

  function handleWindowResize() {
    updateDocumentOverflow();
    queuePreviewGeometry();
    if (runtime.deviceScale !== window.devicePixelRatio) {
      runtime.deviceScale = window.devicePixelRatio;
      runtime.lastContentHeight = null;
      runtime.lastContentLayoutGeneration = null;
      queueContentSize();
    }
  }

  /** @param {PanoramaSnapshot} snapshot */
  function render(snapshot) {
    if (snapshot.version !== protocolVersion || snapshot.kind !== 'snapshot') {
      runtime.bridgeFailed = true;
      drawApp();
      return;
    }
    runtime.snapshot = snapshot;
    runtime.pageGeneration = snapshot.pageGeneration;
    runtime.bridgeFailed = false;
    drawApp();
    queueContentSize();
    queuePreviewGeometry();
  }

  window.addEventListener('error', () => {
    if (!runtime.bridgeFailed) {
      runtime.bridgeFailed = true;
      drawApp();
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
