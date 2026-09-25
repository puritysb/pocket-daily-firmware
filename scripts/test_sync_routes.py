"""Source boundary: advertised data-pack routes cannot depend on a bearer.

This is not a hardware HTTP test. It prevents moving the production route
registrations back under a profile/WS condition, the private-AP regression.
"""
import re
import unittest
from pathlib import Path


class SyncRoutesTest(unittest.TestCase):
    def test_games_are_not_part_of_the_reader_surface(self):
        root = Path(__file__).resolve().parents[1]
        for path in ("src/activities/ActivityManager.cpp", "src/activities/ActivityManager.h",
                     "src/activities/home/HomeActivity.cpp", "src/activities/home/HomeActivity.h"):
            text = (root / path).read_text()
            self.assertNotIn("HomeMenuItem::GAMES", text)
            self.assertNotIn("goToGames", text)
            self.assertNotIn("STR_GAMES_TITLE", text)
        self.assertNotIn("add_subdirectory(games)", (root / "test/CMakeLists.txt").read_text())
        for path in ("src/games/GameModels.h", "src/activities/games/GamesActivity.cpp"):
            self.assertFalse((root / path).exists())

    def test_pocket_connection_guidance_is_separate_from_browser_transfer(self):
        root = Path(__file__).resolve().parents[1]
        chooser = (root / "src/activities/network/NetworkModeSelectionActivity.cpp").read_text()
        for key in ("STR_POCKET_WIFI", "STR_POCKET_DIRECT", "STR_POCKET_WIFI_DESC", "STR_POCKET_DIRECT_DESC"):
            self.assertIn(key, chooser)
        self.assertIn("pocketSync ? NetworkMode::CREATE_HOTSPOT : NetworkMode::CONNECT_CALIBRE", chooser)
        activity = (root / "src/activities/network/CrossPointWebServerActivity.cpp").read_text()
        view = activity[activity.index("void CrossPointWebServerActivity::renderServerRunning()"):
                        activity.index("void CrossPointWebServerActivity::renderNearbySync()")]
        pocket = view[view.index("// Pocket Sync serves"):view.index("} else if (isApMode)")]
        self.assertIn("STR_POCKET_WIFI_ACTION", pocket)
        self.assertIn("STR_POCKET_DIRECT_ACTION", pocket)
        self.assertNotIn("QrUtils", pocket)
        self.assertNotIn("STR_OPEN_URL_HINT", pocket)
        self.assertIn("STR_OPEN_URL_HINT", view)  # Browser mode still works.

    def test_sync_dispatch_and_legacy_registry_share_route_definitions(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        self.assertIn("if (!isSyncProfile(d.profile)) configurePocketRoutes(server, server, d);", source)
        self.assertIn("configurePocketRoutes(routes, server, d);", source)
        self.assertIn("if (!isSyncProfile(d.profile)) return false;", source)
        body = source[source.index("void configurePocketRoutes("):source.index("void registerPocketRoutes(")]
        self.assertNotIn("server.on(", body)
        paths = re.findall(r'routes.on\("([^"]+)"', body)
        self.assertIn("/api/pocket/v1/content/present", paths)
        self.assertIn("/api/pocket/v1/commit", paths)
        host = (root / "src/network/CrossPointWebServer.cpp").read_text()
        self.assertIn("if (PocketDaily::Web::dispatchPocketRoute(*server, pocketRoutes)) return;", host)

    def test_saved_sync_return_precedes_mode_chooser_and_marker_precedes_flash(self):
        root = Path(__file__).resolve().parents[1]
        activity = (root / "src/activities/network/CrossPointWebServerActivity.cpp").read_text()
        entry = activity[activity.index("void CrossPointWebServerActivity::onEnter()"):
                         activity.index("void CrossPointWebServerActivity::returnToLaunchOrigin()")]
        self.assertLess(entry.index("if (autoJoinSavedNetwork)"),
                        entry.index("if (launchMode == WebServerLaunchMode::POCKET_NEARBY_SYNC)"))
        endpoints = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        flash = endpoints[endpoints.index("void handleDevFlash("):]
        self.assertLess(flash.index("if (!saved)"), flash.index("server.send(200"))
        self.assertLess(flash.index("if (!saved)"), flash.index("firmware_flash::flashFromSdPath"))

    def test_presentation_preparation_is_outside_http_and_waits_for_upload_release(self):
        root = Path(__file__).resolve().parents[1]
        activity = (root / "src/activities/network/CrossPointWebServerActivity.cpp").read_text()
        self.assertIn("contentPresentation.enqueue(revision, generation)", activity)
        self.assertNotIn("contentPresentation.prepare(", activity)
        self.assertLess(activity.index("webServer->handleClient();"),
                        activity.index("contentPresentation.service("))
        self.assertIn("contentPresentation.preparationPending() && webServer->presentationTransportIdle()", activity)

    def test_sync_releases_own_time_wait_before_presentation_admission(self):
        root = Path(__file__).resolve().parents[1]
        host = (root / "src/network/CrossPointWebServer.cpp").read_text()
        loop = host[host.index("void CrossPointWebServer::handleClient() {"):]
        purge = loop.index("pocketTimeWait.service(millis(), port, pocketStream.port(), admission);")
        # Sync only, and ahead of the presentation-busy early return so the
        # activity's admission sample (right after handleClient) follows a purge.
        self.assertLess(loop.index("if (PocketDaily::Web::isSyncProfile(profile)) {"), purge)
        self.assertLess(purge, loop.index("if (pocketRoutes.presentation.busy && pocketRoutes.presentation.busy("))
        walker = (root / "src/pocket_daily/web/ServerTimeWait.cpp").read_text()
        self.assertIn("LOCK_TCPIP_CORE();", walker)
        self.assertIn("purgeTimeWaitList(tcp_tw_pcbs, httpPort, streamPort", walker)
        self.assertNotIn("tcp_active_pcbs", walker)  # never touches live or closing connections
        census = (root / "src/pocket_daily/web/TcpCensus.cpp").read_text()
        self.assertTrue(census.startswith('#include "TcpCensus.h"\n\n#ifdef ENABLE_DEV_REMOTE_FLASH'))

    def test_pocket_profile_drives_home_sleep_and_is_loaded_before_use(self):
        root = Path(__file__).resolve().parents[1]
        activity = (root / "src/activities/pocket_daily/PocketDailyActivity.cpp").read_text()
        collect = activity[activity.index("int PocketDailyActivity::collectOverview("):
                           activity.index("bool PocketDailyActivity::hasMonitorData()")]
        # Order, visibility and the daily-word fallback come only from the
        # shared homeRowSources, which the host preview uses too (HomeRowSources
        # host test); the activity only appends rows for the resolved sources.
        self.assertIn("PocketDaily::Home::homeRowSources(PocketDaily::DailyProfile::current(), available, sources)",
                      collect)
        for source in ("Reading", "AppCards", "DailyWord", "Provider", "Monitor"):
            self.assertIn(f"RowSource::{source}:", collect)
        self.assertNotIn("homeItems", collect)
        shared = (root / "src/pocket_daily/home/HomeRenderer.cpp").read_text()
        self.assertIn("for (uint8_t k = 0; k < profile.homeCount && k < DailyProfile::HOME_ITEM_CAP; ++k)", shared)
        self.assertIn("else if (profile.dailyWord && !profile.shows(HomeItem::Word))", shared)
        sleep = activity[activity.index("bool PocketDailyActivity::paintSleepFrame() {"):]
        self.assertLess(sleep.index("SleepMode::Reader) return false;"), sleep.index("requestUpdateAndWait();"))
        boot = (root / "src/pocket_daily/boot/ProductBoot.cpp").read_text()
        self.assertIn("PocketDaily::DailyProfile::loadAtBoot();", boot)
        endpoints = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        body = endpoints[endpoints.index("void configurePocketRoutes("):endpoints.index("void registerPocketRoutes(")]
        self.assertIn('routes.on("/api/pocket/v1/profile", HTTP_GET', body)
        self.assertIn('routes.on("/api/pocket/v1/profile", HTTP_POST', body)

    def test_theme_routes_are_unconditional_in_production_registration(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        body = source[source.index("void configurePocketRoutes("):source.index("void registerPocketRoutes(")]
        for route in ("/api/pocket/v1/ui-packs", "/api/pocket/v1/ui-pack/apply"):
            with self.subTest(route=route):
                marker = 'routes.on("' + route + '"'
                self.assertEqual(body.count(marker), 1)
                prefix = body[:body.index(marker)]
                # Ignore comments and quoted literals before counting lexical
                # scope. Registration must sit at function scope, not in an if.
                prefix = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                                "", prefix, flags=re.S)
                self.assertEqual(prefix.count("{") - prefix.count("}"), 1)

    def test_content_file_reads_only_published_leaves_and_never_writes(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        body = source[source.index("void configurePocketRoutes("):source.index("void registerPocketRoutes(")]
        self.assertEqual(body.count('routes.on("/api/pocket/v1/content/file", HTTP_GET'), 1)
        handler = source[source.index("void handleContentFile("):source.index("void handleGetPreferences(")]
        self.assertIn("admitContentOperation(server, d, deviceId)", handler)
        self.assertIn("d.stream->transferActive()", handler)
        self.assertIn("Content::publishedFilePath(", handler)
        for write in ("FILE_WRITE", "O_WRITE", ".remove(", ".rename(", "Storage.mkdir"):
            self.assertNotIn(write, handler)
        status = (root / "src/pocket_daily/web/PocketStatus.cpp").read_text()
        self.assertIn('doc["contentRead"] = 1;', status)


if __name__ == "__main__":
    unittest.main()
