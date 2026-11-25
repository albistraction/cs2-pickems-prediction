'use strict';
// Timers em Web Worker não sofrem o "throttling" de abas em segundo plano.
self.onmessage = (e) => {
  const { id, ms } = e.data;
  setTimeout(() => self.postMessage(id), ms);
};
