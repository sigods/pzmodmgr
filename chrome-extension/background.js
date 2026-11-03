const HOST = "pzmodmgr";
const ICON = "icons/icon128.png";

function isWorkshop(url) {
  try {
    const parsed = new URL(url);
    const id = parsed.searchParams.get("id");
    return parsed.hostname === "steamcommunity.com" &&
           parsed.pathname === "/sharedfiles/filedetails/" &&
           /^\d+$/.test(id || "");
  } catch (error) {
    return false;
  }
}

function notify(title, message) {
  const text = String(message || "");
  chrome.notifications.create({
    type: "basic",
    iconUrl: ICON,
    title,
    message: text.length > 300 ? text.slice(0, 297) + "..." : text
  });
}

chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({
      id: "pzmodmgr-add-page",
      title: "Add to PZ Mod Manager",
      contexts: ["page"],
      documentUrlPatterns: ["*://steamcommunity.com/sharedfiles/filedetails/*"]
    });
    chrome.contextMenus.create({
      id: "pzmodmgr-add-link",
      title: "Add to PZ Mod Manager",
      contexts: ["link"],
      targetUrlPatterns: ["*://steamcommunity.com/sharedfiles/filedetails/*"]
    });
  });
});

chrome.contextMenus.onClicked.addListener((info, tab) => {
  const url = info.linkUrl || info.pageUrl || (tab && tab.url);
  if (!isWorkshop(url)) {
    notify("PZ Mod Manager", "This is not a Steam Workshop item or collection page.");
    return;
  }

  notify("PZ Mod Manager", "Adding Workshop item...");
  chrome.runtime.sendNativeMessage(HOST, { url }, (response) => {
    if (chrome.runtime.lastError) {
      notify("PZ Mod Manager failed", chrome.runtime.lastError.message);
      return;
    }
    if (response && response.ok) {
      notify("PZ Mod Manager", response.message || "Installed.");
    } else {
      notify("PZ Mod Manager failed",
             (response && response.message) || "No response from the native host.");
    }
  });
});
