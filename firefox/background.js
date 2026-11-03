const HOST = "pzmodmgr";

function isWorkshop(url) {
  try {
    const u = new URL(url);
    const id = u.searchParams.get("id");
    return u.hostname === "steamcommunity.com" &&
           u.pathname === "/sharedfiles/filedetails/" &&
           /^\d+$/.test(id || "");
  } catch (e) {
    return false;
  }
}

function notify(title, message) {
  const text = String(message || "");
  browser.notifications.create({
    type: "basic",
    title,
    message: text.length > 300 ? text.slice(0, 297) + "..." : text
  });
}

browser.menus.removeAll().then(() => {
  browser.menus.create({
    id: "pzmodmgr-add-page",
    title: "Add to PZ Mod Manager",
    contexts: ["page"],
    targetUrlPatterns: ["*://steamcommunity.com/sharedfiles/filedetails/*"]
  });
  browser.menus.create({
    id: "pzmodmgr-add-link",
    title: "Add to PZ Mod Manager",
    contexts: ["link"],
    targetUrlPatterns: ["*://steamcommunity.com/sharedfiles/filedetails/*"]
  });
});

browser.menus.onClicked.addListener((info, tab) => {
  const url = info.linkUrl || info.pageUrl || (tab && tab.url);
  if (!isWorkshop(url)) {
    notify("PZ Mod Manager", "This is not a Steam Workshop item or collection page.");
    return;
  }

  notify("PZ Mod Manager", "Adding Workshop item...");
  browser.runtime.sendNativeMessage(HOST, { url })
    .then((response) => {
      if (response && response.ok) {
        notify("PZ Mod Manager", response.message || "Installed.");
      } else {
        notify("PZ Mod Manager failed", (response && response.message) || "No response from the native host.");
      }
    })
    .catch((error) => {
      notify("PZ Mod Manager failed", String(error));
    });
});
