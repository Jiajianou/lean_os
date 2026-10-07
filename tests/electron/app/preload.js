'use strict';

const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('lean', {
  ask: (n) => ipcRenderer.invoke('lean:double', n),
});
