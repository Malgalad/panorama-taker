(() => {
  'use strict';
  const protocolVersion = 1;
  let pageGeneration = 0;
  let contentSizeFrame = 0;
  let lastContentHeight = null;
  let pendingExposure = null;

  const byId = id => document.getElementById(id);
  const post = (kind, fields = {}) => window.chrome.webview.postMessage({
    version: protocolVersion,
    kind,
    pageGeneration,
    ...fields
  });
  const formatEv = value => `${value > 0 ? '+' : ''}${value.toFixed(1)} EV`;

  function reportContentSize() {
    contentSizeFrame = 0;
    const height = Math.ceil(document.querySelector('.exposure-root').getBoundingClientRect().height);
    if (!Number.isFinite(height) || height <= 0 || height === lastContentHeight) return;
    lastContentHeight = height;
    post('content-size', { layoutGeneration: 0, height });
  }

  function queueContentSize() {
    if (!contentSizeFrame) contentSizeFrame = requestAnimationFrame(reportContentSize);
  }

  function render(snapshot) {
    if (snapshot.version !== protocolVersion || snapshot.kind !== 'exposure-snapshot') return;
    pageGeneration = snapshot.pageGeneration;
    byId('show-overlay').checked = snapshot.overlay;
    byId('show-overlay').disabled = snapshot.busy;
    if (pendingExposure !== null &&
        Math.abs(snapshot.finalExposure - pendingExposure) < 0.001) {
      pendingExposure = null;
    }
    const displayedExposure = pendingExposure ?? snapshot.finalExposure;
    byId('final-exposure').value = String(displayedExposure);
    byId('final-exposure').disabled = snapshot.busy;
    byId('final-exposure-value').textContent = formatEv(displayedExposure);
    byId('reference').textContent = snapshot.reference === null
      ? '<none>'
      : String(snapshot.reference + 1);
    const poses = byId('poses');
    poses.replaceChildren(...Array.from({ length: snapshot.poseCount }, (_, index) => {
      const pose = document.createElement('div');
      pose.className = `pose${snapshot.reference === index ? ' reference' : ''}${snapshot.selected.includes(index) ? ' manual' : ''}`;
      pose.textContent = String(index + 1);
      pose.tabIndex = snapshot.busy ? -1 : 0;
      pose.setAttribute('role', 'button');
      pose.setAttribute('aria-disabled', String(snapshot.busy));
      pose.addEventListener('click', () => post('set-exposure-reference', { index }));
      pose.addEventListener('keydown', event => {
        if (event.key !== 'Enter' && event.key !== ' ') return;
        event.preventDefault();
        post('set-exposure-reference', { index });
      });
      pose.addEventListener('contextmenu', event => {
        event.preventDefault();
        post('toggle-exposure-selection', { index });
      });
      pose.addEventListener('pointerenter', () => post('hover-exposure-pose', { index }));
      pose.addEventListener('pointerleave', () => post('clear-exposure-hover'));
      return pose;
    }));
    const hasSelection = snapshot.reference !== null || snapshot.selected.length !== 0;
    byId('reset').disabled = snapshot.busy || !hasSelection;
    byId('equalize').disabled = snapshot.busy || snapshot.reference === null;
    byId('equalize').textContent = snapshot.selected.length === 0
      ? 'Equalize all'
      : 'Equalize selected';
    queueContentSize();
  }

  byId('show-overlay').addEventListener('change', event =>
    post('set-exposure-overlay', { enabled: event.currentTarget.checked }));
  byId('final-exposure').addEventListener('input', event => {
    const value = Number(event.currentTarget.value);
    pendingExposure = value;
    byId('final-exposure-value').textContent = formatEv(value);
  });
  byId('final-exposure').addEventListener('change', event =>
    post('set-final-exposure', { value: Number(event.currentTarget.value) }));
  byId('reset').addEventListener('click', () => post('reset-exposure'));
  byId('equalize').addEventListener('click', () => post('equalize-exposure'));
  window.chrome.webview.addEventListener('message', event => render(event.data));
  window.addEventListener('resize', () => {
    lastContentHeight = null;
    queueContentSize();
  });
  post('ready');
})();
