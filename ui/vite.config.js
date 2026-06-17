import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'

// Inline JS+CSS into a single self-contained index.html. Essential for WKWebView:
// over file:// (origin "null") it blocks module/crossorigin sibling files, so a
// single inlined file is the reliable way to load the UI.
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  base: './',
  build: { outDir: 'dist', emptyOutDir: true, assetsInlineLimit: 100000000 },
})
