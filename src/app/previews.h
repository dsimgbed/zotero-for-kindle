#pragma once
// The app's side of zk.reader.open (0.9), kept out of main.cpp: what PluginEvents opens a preview
// with, the reader a previewed document gets, and (desktop, dev only) a plugin screen drawn for
// --screenshot. Nothing here takes or returns JSON, so main.cpp never builds a JSON value itself
// (the desktop build's main.cpp may see another nlohmann/json than the libraries).

#include <functional>
#include <memory>
#include <string>

#include "app/plugin_events.h"
#include "docs/document.h"
#include "screens/preview.h"
#include "screens/reader.h"
#include "update/version.h"

namespace zk {

class Lipc;
class PluginHost;

// A previewed document in the reader: not an item, so no item key; a page the plugin had reads as
// a web snapshot.
std::unique_ptr<ReaderScreen> previewReader(const PreviewEntry&, const EngineEnv&, int flashEvery, Lipc*);

// Opens a ReaderOpen over what shows (PreviewScreen::show): `cache` keeps the documents, `host()`
// downloads under the plugin's net rules (the current host: nullptr when plugins aren't running),
// `makeReader` makes the reader.
PluginEvents::PreviewOpener previewOpener(PreviewCache& cache, std::function<PluginHost*()> host,
                                          std::function<std::unique_ptr<ReaderScreen>(const PreviewEntry&)> makeReader);

// Dev only (desktop --screenshot plugin:ID:SCREEN[:page]): loads the plugins in `pluginsDir` with
// a host of its own over the library at `dbPath`, opens the screen, and writes it to `out` as
// drawn, after `page` page turns. 0 on success.
int pluginScreenshot(const std::string& pluginsDir, const std::string& dbPath, const EngineEnv&, Version,
                     const std::string& id, const std::string& screenId, int page, const std::string& out);

// Dev only (desktop --screenshot home --home ID/SCREEN): Home as the app draws it with that
// plugin screen as its body (Settings → Home), through the app's own HomeScreen and PluginHome, once
// the plugin's first tree has come (or after 5 s: 1, Home's built-in body drawn). 0 on success.
int homeScreenshot(const std::string& pluginsDir, const std::string& dbPath, const EngineEnv&, Version,
                   const std::string& id, const std::string& screenId, const std::string& out);

}  // namespace zk
