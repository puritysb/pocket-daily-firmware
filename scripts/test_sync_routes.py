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

    def test_articles_live_under_pocket_reader_not_home(self):
        # Home stays the stock CrossPoint menu plus the single Pocket Daily
        # entry; the Articles library is reached from Pocket Daily's Home.
        root = Path(__file__).resolve().parents[1]
        home = (root / "src/activities/home/HomeActivity.cpp").read_text()
        manager = (root / "src/activities/ActivityManager.h").read_text()
        pocket = (root / "src/activities/pocket_daily/PocketDailyActivity.cpp").read_text()
        articles = (root / "src/activities/home/ArticlesActivity.cpp").read_text()
        self.assertNotIn("STR_ARTICLES", home)
        self.assertNotIn("HomeMenuItem::ARTICLES", manager)
        self.assertIn("activityManager.goToArticles()", pocket)
        self.assertIn("activityManager.goToPocketDaily()", articles)

    def test_books_return_to_the_shell_that_opened_them(self):
        # Pocket Daily and Articles record themselves as the book's origin; the
        # reader's Back and end-of-book exits go through leaveReader, and any
        # trip Home clears the origin so a book opened there returns Home.
        root = Path(__file__).resolve().parents[1]
        pocket = (root / "src/activities/pocket_daily/PocketDailyActivity.cpp").read_text()
        articles = (root / "src/activities/home/ArticlesActivity.cpp").read_text()
        reader = (root / "src/activities/reader/ReaderActivity.cpp").read_text()
        manager = (root / "src/activities/ActivityManager.cpp").read_text()
        self.assertIn("goToReaderFrom(ReaderReturn::PocketDaily", pocket)
        self.assertIn("goToReaderFrom(ReaderReturn::Articles", articles)
        self.assertNotIn("onGoHome()", reader)
        self.assertEqual(reader.count("activityManager.leaveReader()"), 3)
        go_home = manager[manager.index("void ActivityManager::goHome("):]
        self.assertIn("readerReturn = ReaderReturn::Home;", go_home[:go_home.index("\n}")])
        # The first front button leads to CrossPoint Home and says so.
        self.assertIn("mapLabels(tr(STR_POCKET_HOME)", pocket)
        self.assertNotIn("STR_POCKET_LIBRARY", pocket)

    def test_stock_screens_do_not_host_reading_sync_windows(self):
        root = Path(__file__).resolve().parents[1]
        for path in ("src/activities/home/HomeActivity.h", "src/activities/home/FileBrowserActivity.h",
                     "src/activities/library/LibraryListActivity.h"):
            self.assertNotIn("allowsExchangeWindow", (root / path).read_text(), path)
        for path in ("src/activities/pocket_daily/PocketDailyActivity.h",
                     "src/activities/boot_sleep/SleepActivity.h"):
            self.assertIn("allowsExchangeWindow() const override { return true; }", (root / path).read_text(), path)

    def test_page_turns_defer_the_progress_write(self):
        # renderBook() queues the position; loop() writes it after PROGRESS_SAVE_IDLE_MS
        # and onExit flushes it. A synchronous save in renderBook() (the 1.6.5 merge
        # regression) makes every turn wait on the SD rename again.
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/activities/reader/EpubReaderActivity.cpp").read_text()
        render = source[source.index("void EpubReaderActivity::renderBook()"):]
        render = render[:render.index("\nvoid EpubReaderActivity::")]
        self.assertTrue("progressSavePending = true;" in render, "renderBook() must queue the save")
        self.assertFalse("saveProgress(currentSpineIndex" in render, "renderBook() writes progress synchronously")

    def test_pocket_connection_guidance_is_separate_from_browser_transfer(self):
        root = Path(__file__).resolve().parents[1]
        chooser = (root / "src/activities/network/NetworkModeSelectionActivity.cpp").read_text()
        for key in ("STR_POCKET_WIFI", "STR_POCKET_DIRECT", "STR_POCKET_WIFI_DESC", "STR_POCKET_DIRECT_DESC"):
            self.assertIn(key, chooser)
        # Pocket's second item is the direct (private AP) path, never Calibre;
        # its third is the release update check, absent from File Transfer.
        compact = re.sub(r"\s+", " ", chooser)
        self.assertIn("POCKET_MODES[] = {NetworkMode::JOIN_NETWORK, NetworkMode::CREATE_HOTSPOT, "
                      "NetworkMode::CHECK_FOR_UPDATES}", compact)
        self.assertIn("TRANSFER_MODES[] = {NetworkMode::JOIN_NETWORK, NetworkMode::CONNECT_CALIBRE, "
                      "NetworkMode::CREATE_HOTSPOT, NetworkMode::USB_DRIVE}", compact)
        self.assertIn("pocketSync ? POCKET_MODES[index] : TRANSFER_MODES[index]", chooser)
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
        self.assertIn("/api/pocket/v1/transfer", paths)
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
        # Card pages and Home / Daily Brief share one slot; HTTP only queues.
        self.assertIn("presentation.enqueueContent(revision, generation, activity.renderer)", activity)
        self.assertIn("presentation.enqueueScreen(surface, generation, activity.renderer)", activity)
        self.assertNotIn(".prepare(", activity)
        self.assertLess(activity.index("webServer->handleClient();"),
                        activity.index("presentation.service("))
        self.assertIn("presentation.preparationPending() && webServer->presentationTransportIdle()", activity)

    def test_sync_releases_own_time_wait_before_presentation_admission(self):
        root = Path(__file__).resolve().parents[1]
        host = (root / "src/network/CrossPointWebServer.cpp").read_text()
        loop = host[host.index("void CrossPointWebServer::handleClient() {"):]
        purge = loop.index("pocketTimeWait.service(millis(), port, pocketStream.port(), admission);")
        # Sync only, and ahead of the presentation-busy early return so the
        # activity's admission sample (right after handleClient) follows a purge.
        self.assertLess(loop.index("if (PocketDaily::Web::isSyncProfile(profile)) {"), purge)
        self.assertLess(purge, loop.index("if (presentationBusy()) {"))
        # The busy signal covers card pages and Home / Daily Brief presentation.
        busy = host[host.index("bool CrossPointWebServer::presentationBusy() const {"):]
        busy = busy[:busy.index("\n}\n")]
        self.assertIn("pocketRoutes.presentation.busy(pocketRoutes.presentation.self)", busy)
        self.assertIn("pocketRoutes.screen.busy(pocketRoutes.screen.self)", busy)
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
                           activity.index("uint32_t PocketDailyActivity::studyEpoch()")]
        # Order, visibility and the daily-word fallback come only from the
        # shared homeRowSources, which the host preview uses too (HomeRowSources
        # host test); the activity only appends rows for the resolved sources.
        self.assertIn("PocketDaily::Home::homeRowSources(PocketDaily::DailyProfile::current(), available, sources)",
                      collect)
        for source in ("Reading", "AppCards", "DailyWord"):
            self.assertIn(f"RowSource::{source}:", collect)
        # Provider and monitor (AgentDeck daemon data) are retired: they still
        # parse in stored profiles but resolve to no Home source.
        for retired in ("Provider", "Monitor"):
            self.assertNotIn(f"RowSource::{retired}", collect)
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

    def test_app_glance_route_is_admitted_validated_then_stored(self):
        root = Path(__file__).resolve().parents[1]
        endpoints = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        body = endpoints[endpoints.index("void configurePocketRoutes("):endpoints.index("void registerPocketRoutes(")]
        # Registered on the Sync profiles, in the same block as the profile.
        sync = body[body.index("if (isSyncProfile(d.profile)) {"):]
        sync = sync[:sync.index("\n  }\n")]
        self.assertIn('routes.on("/api/pocket/v1/profile", HTTP_POST', sync)
        self.assertIn('routes.on("/api/pocket/v1/glance", HTTP_POST', sync)
        self.assertNotIn('"/api/pocket/v1/glance", HTTP_GET', endpoints)
        handler = endpoints[endpoints.index("void handlePostGlance("):endpoints.index("void retireContent(")]
        # Identity + heap admission first, the whole document parsed before
        # anything is stored, and the clock set only after a stored glance.
        admit = handler.index("if (!admitContentOperation(server, d, deviceId)) return;")
        parse = handler.index("AppGlance::parseJson(")
        save = handler.index("AppGlance::save(")
        clock = handler.index("settimeofday(")
        self.assertLess(admit, parse)
        self.assertLess(parse, save)
        self.assertLess(save, clock)
        self.assertIn("time(nullptr) < static_cast<time_t>(AppGlance::MIN_EPOCH)", handler)
        status = (root / "src/pocket_daily/web/PocketStatus.cpp").read_text()
        self.assertIn('if (isSyncProfile(in.profile)) doc["pocketGlance"] = 1;', status)
        activity = (root / "src/activities/pocket_daily/PocketDailyActivity.cpp").read_text()
        enter = activity[activity.index("void PocketDailyActivity::onEnter() {"):
                         activity.index("void PocketDailyActivity::loop() {")]
        self.assertLess(enter.index("PocketDaily::AppGlance::load(glanceSnapshot)"), enter.index("requestUpdate();"))
        # No daemon, discovery or radio bring-up is left in Pocket Daily.
        for gone in ("AgentDeck::", "WiFi.begin(", "WiFi.scanNetworks(", "MDNS", "enterTimedDeepSleep"):
            self.assertNotIn(gone, activity)
        self.assertFalse((root / "src/agentdeck").exists())

    def test_ui_packs_are_not_part_of_the_firmware(self):
        # The companion dropped .uipack editing on 2026-09-25 (app a98e879);
        # themes read their own metrics and no route or status field offers packs.
        root = Path(__file__).resolve().parents[1]
        routes = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        for route in ("/api/pocket/v1/ui-packs", "/api/pocket/v1/ui-pack/apply"):
            self.assertNotIn(route, routes)
        status = (root / "src/pocket_daily/web/PocketStatus.cpp").read_text()
        for field in ('"uiPacks"', '"activePack"', '"activePackVersion"'):
            self.assertNotIn(field, status)
        self.assertNotIn("adoptPackMetrics", (root / "src/components/UITheme.h").read_text())
        self.assertFalse((root / "src/pocket_daily/live_studio/UiPackStore.cpp").exists())

    def test_stock_themes_carry_no_product_code(self):
        # Themes stay upstream's except per-label CJK fonts (UiCjkFont), the
        # status-bar title fit and minimal Home menu paging; Pocket screens draw
        # their own content (docs/fork-delta-register.md).
        root = Path(__file__).resolve().parents[1]
        for path in sorted((root / "src/components").rglob("*")):
            if path.suffix not in (".h", ".cpp"):
                continue
            text = path.read_text()
            self.assertNotIn("pocket_daily/", text, path.name)
            for gone in ("drawList(", "drawTabBar(", "drawKeyboardKey(", "drawContentPage(", "adoptPackMetrics"):
                self.assertNotIn(gone, text, f"{path.name}: {gone}")

    def test_reader_file_download_is_sync_only_and_advertised(self):
        # docs/reader-files.md: piece download is a Sync route next to the file
        # listing, advertised as readerFiles: 2, one bounded write per request.
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        body = source[source.index("void configurePocketRoutes("):source.index("void registerPocketRoutes(")]
        sync = body[body.index("if (isSyncProfile(d.profile)) {"):]
        self.assertIn('routes.on("/api/pocket/v1/files/content", HTTP_GET', sync)
        handler = source[source.index("void handleReaderFileContent("):source.index("enum class ReaderFileAction")]
        self.assertIn("admitContentOperation", handler)
        self.assertIn("downloadableReaderFile", handler)
        self.assertIn("firmware_flash::sharedStagingBuffer()", handler)
        # The tested helpers carry the data path: bearer piece limit, offset read.
        self.assertIn("downloadPieceLimit(d.profile == Profile::POCKET_SYNC)", handler)
        self.assertIn("readDownloadPiece(file, offset, body, piece.length)", handler)
        self.assertNotIn("CONTENT_LENGTH_UNKNOWN", handler)
        status = (root / "src/pocket_daily/web/PocketStatus.cpp").read_text()
        self.assertIn('doc["readerFiles"] = 2;', status)

    def test_live_frame_capture_is_not_part_of_the_firmware(self):
        # The companion dropped reader-screen capture on 2026-09-25; the render
        # loop publishes no frames and the status/push channel stays.
        root = Path(__file__).resolve().parents[1]
        routes = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        for route in ("/api/pocket/v1/screen-live", "/api/pocket/v1/dev/capture", "/api/pocket/v1/dev/frame"):
            self.assertNotIn(route, routes)
        self.assertNotIn("LiveFrameCapture", (root / "src/activities/ActivityManager.cpp").read_text())
        status = (root / "src/pocket_daily/web/PocketStatus.cpp").read_text()
        self.assertNotIn('"frameStream"', status)
        # The companion decodes liveStudio.mode as required.
        self.assertIn('live["mode"]', status)
        # The saved Pocket screen preview went with it.
        self.assertNotIn("/api/pocket/v1/screen-preview", routes)
        self.assertNotIn('"screenPreviewAvailable"', status)

    def test_screen_presentation_is_sync_only_identity_first_and_advertised(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        body = source[source.index("void configurePocketRoutes("):source.index("void registerPocketRoutes(")]
        block = body[body.index("if (isSyncProfile(d.profile) && d.screen.enqueue && d.screen.state"):]
        block = block[:block.index("\n  }\n")]
        self.assertIn('routes.on("/api/pocket/v1/screen/present", HTTP_POST', block)
        self.assertIn('routes.on("/api/pocket/v1/screen/presentation", HTTP_GET', block)
        handler = source[source.index("void handleScreenPresentation("):source.index("void handleCrashReport(")]
        identity = handler.index('server.arg("deviceID") != deviceId')
        self.assertLess(identity, handler.index('server.arg("surface")'))
        self.assertLess(handler.index('server.arg("surface")'), handler.index('server.arg("generation")'))
        paint = handler[handler.index("if (requestPaint) {"):]
        self.assertLess(paint.index("admitContentOperation(server, d, deviceId)"),
                        paint.index("d.screen.enqueue("))
        self.assertLess(paint.index("DailyProfile::generation() != generation"), paint.index("d.screen.enqueue("))
        self.assertIn("SleepMode::Reader", paint[:paint.index("d.screen.enqueue(")])
        # The receipt read never touches SD or the render lock.
        read = handler[handler.index("const auto receipt = d.screen.state(d.screen.self);"):]
        for heavy in ("Storage.", "RenderLock", "recoverActiveRevision"):
            self.assertNotIn(heavy, read)
        status = (root / "src/pocket_daily/web/PocketStatus.cpp").read_text()
        self.assertIn('if (in.screenPresentation) doc["screenPresentation"] = 1;', status)
        host = (root / "src/network/CrossPointWebServer.cpp").read_text()
        self.assertIn("in.screenPresentation = PocketDaily::Web::isSyncProfile(profile) && pocketRoutes.screen.enqueue",
                      host)

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

    def test_reading_progress_routes_read_records_and_only_queue_offers(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/pocket_daily/web/PocketEndpoints.cpp").read_text()
        body = source[source.index("void configurePocketRoutes("):source.index("void registerPocketRoutes(")]
        sync = body[body.index("if (isSyncProfile(d.profile)) {"):]
        sync = sync[:sync.index("\n  }\n")]
        self.assertIn('routes.on("/api/pocket/v1/reading", HTTP_GET', sync)
        self.assertIn('routes.on("/api/pocket/v1/reading", HTTP_POST', sync)
        status = (root / "src/pocket_daily/web/PocketStatus.cpp").read_text()
        self.assertIn('if (isSyncProfile(in.profile)) doc["readingProgress"] = 1;', status)
        handlers = source[source.index("void handleReadingList("):source.index("void handleTransferControl(")]
        # HTTP and BLE (READ_LIST / OFFER) share one exchange implementation.
        exchange = (root / "src/pocket_daily/ReadingExchange.cpp").read_text()
        session = (root / "src/pocket_daily/nearby_sync/ReadingSyncSession.cpp").read_text()
        # No chapter streaming or position mapping while serving an exchange: the
        # reader computes XPointers when a book is left (ReadingProgressReader).
        for heavy in ("ChapterXPathResolver", "ProgressMapper", "readSpineItemToStream", "readItemContentsToStream",
                      ".load(", "O_WRITE", "saveProgress(", "saveRecord("):
            self.assertNotIn(heavy, handlers)
            self.assertNotIn(heavy, exchange)
            self.assertNotIn(heavy, session)
        listing = handlers[handlers.index("void handleReadingList("):handlers.index("void handleReadingOffer(")]
        self.assertIn("admitContentOperation(server, d, deviceId)", listing)
        self.assertIn("Reading::ListStream stream(deviceId, true, *work)", listing)
        self.assertNotIn("saveOffer", listing)
        self.assertIn("stream(deviceId, false, work)", session)  # no paths over the air
        composer = (root / "src/pocket_daily/ReadingProgress.cpp").read_text()
        self.assertIn("MAX_LIST_BYTES", composer[composer.index("size_t ListComposer::entry("):])
        offer = handlers[handlers.index("void handleReadingOffer("):]
        # Parsed and identity-checked before anything is stored; never touches progress.bin.
        self.assertLess(offer.index("Reading::parseOfferJson("), offer.index("admitOperationFor("))
        self.assertLess(offer.index("admitOperationFor("), offer.index("Reading::storeOffer("))
        self.assertNotIn("saveProgress(", offer)
        ble = session[session.index("void ReadingSyncSession::completeOffer("):]
        self.assertLess(ble.index("Reading::parseOfferJson("), ble.index("service.deviceId()"))
        self.assertLess(ble.index("service.deviceId()"), ble.index("Reading::storeOffer("))
        store = exchange[exchange.index("OfferStoreResult storeOffer("):]
        self.assertIn("saveOffer(", store)
        reader = (root / "src/activities/reader/EpubReaderActivity.cpp").read_text()
        exit_hook = reader[reader.index("EpubReaderActivity::~EpubReaderActivity()"):reader.index("void EpubReaderActivity::loop()")]
        self.assertLess(exit_hook.index("capturePosition("), exit_hook.index("section.reset();"))
        self.assertLess(exit_hook.index("section.reset();"), exit_hook.index("recordPosition("))
        self.assertLess(exit_hook.index("recordPosition("), exit_hook.index("moveFinishedBookToReadFolder("))


if __name__ == "__main__":
    unittest.main()
