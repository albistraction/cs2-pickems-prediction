'use strict';
// Clique no ícone da extensão -> abre (ou foca) o painel do coletor.
chrome.action.onClicked.addListener(async () => {
  const url = chrome.runtime.getURL('painel.html');
  const tabs = await chrome.tabs.query({ url });
  if (tabs.length) {
    await chrome.tabs.update(tabs[0].id, { active: true });
    await chrome.windows.update(tabs[0].windowId, { focused: true });
  } else {
    await chrome.tabs.create({ url });
  }
});
