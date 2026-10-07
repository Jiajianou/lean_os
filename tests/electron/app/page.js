'use strict';

// The page itself holds no Node: it reaches the main process only through
// what the preload put on window.lean, which is the shape VS Code's
// workbench has - a sandboxed renderer and a context bridge.
window.lean.ask(21).then((reply) => {
  document.getElementById('answer').textContent =
    JSON.stringify({ doubled: reply.doubled, mainPid: reply.pid, nodeInPage: typeof require });
});
