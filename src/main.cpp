#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <optional>
#include <dankmeme.globed2/include/globed/core/data/PlayerDisplayData.hpp>
#include <dankmeme.globed2/include/globed/core/data/PlayerState.hpp>

using namespace geode::prelude;

// Globed's soft-link API table is not registered in its Windows build, so these are the
// functions its DLL exports directly, declared by hand (GlobedGJBGL has no public header).
namespace globed {

class RemotePlayer;

class VisualPlayer {
public:
    __declspec(dllimport) RemotePlayer* getRemotePlayer();
};

class RemotePlayer : public std::enable_shared_from_this<RemotePlayer> {
public:
    __declspec(dllimport) VisualPlayer* player1();
    __declspec(dllimport) PlayerDisplayData& displayData();
    __declspec(dllimport) int id() const;
    __declspec(dllimport) bool isLocal() const;

    // First member of the real class (private there, see RemotePlayer.hpp in Globed v2.2.2).
    // Only read through spectate::stateOf, which checks that it looks sane.
    PlayerState m_state;
};

struct GlobedGJBGL {
    __declspec(dllimport) static GlobedGJBGL* get(GJBaseGameLayer* base);
    __declspec(dllimport) bool active();
    __declspec(dllimport) std::shared_ptr<RemotePlayer> getPlayer(int id);
    __declspec(dllimport) void toggleCullingEnabled(bool enabled);
};

// Tag Globed puts on every PlayerObject it creates
static constexpr int PLAYER_TAG = 3458738;

} // namespace globed

namespace spectate {

static constexpr auto LABEL_ID = "spectate-label"_spr;

// How far back (in units) the target has to jump in one frame for it to count as a respawn
static constexpr float RESPAWN_JUMP = 150.f;

// How long after a jump in the level the music gets lined up a second time, in case the game
// was still starting the song the first time
static constexpr float MUSIC_RESYNC_DELAY = .3f;

static int g_target = 0;
static std::string g_targetName;
static CCPoint g_lastPos;
static bool g_hasLastPos = false;
static bool g_wasDead = false;
static bool g_musicSyncPending = false;
static float g_musicResyncIn = 0.f;

bool active() {
    return g_target != 0;
}

int target() {
    return g_target;
}

static globed::GlobedGJBGL* globedLayer() {
    auto pl = PlayLayer::get();
    return pl ? globed::GlobedGJBGL::get(pl) : nullptr;
}

// Whether we are in a level with a live Globed session
bool sessionActive() {
    auto gl = globedLayer();
    return gl && gl->active();
}

static std::shared_ptr<globed::RemotePlayer> remoteOf(int id) {
    auto gl = globedLayer();
    return gl ? gl->getPlayer(id) : nullptr;
}

std::string nameOf(int id) {
    if (auto rp = remoteOf(id)) {
        auto& name = rp->displayData().username;
        if (!name.empty()) return name;
    }
    return fmt::format("Player {}", id);
}

// VisualPlayer is a PlayerObject
CCNode* nodeOf(int id) {
    auto rp = remoteOf(id);
    if (!rp) return nullptr;
    return reinterpret_cast<CCNode*>(rp->player1());
}

static void collectPlayers(CCNode* node, int depth, std::vector<int>& out) {
    for (auto child : CCArrayExt<CCNode*>(node->getChildren())) {
        if (child->getTag() == globed::PLAYER_TAG && typeinfo_cast<PlayerObject*>(child)) {
            auto vp = reinterpret_cast<globed::VisualPlayer*>(child);
            auto rp = vp->getRemotePlayer();
            if (rp && !rp->isLocal() && rp->player1() == vp) out.push_back(rp->id());
        } else if (depth > 0 && !typeinfo_cast<CCSpriteBatchNode*>(child)) {
            collectPlayers(child, depth - 1, out);
        }
    }
}

// Globed exports no player list, so find its player objects in the level
std::vector<int> players() {
    std::vector<int> ids;
    auto pl = PlayLayer::get();
    if (!pl || !sessionActive()) return ids;

    collectPlayers(pl, 4, ids);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Globed's last known state of a player, or null if it can't be read
static const globed::PlayerState* stateOf(globed::RemotePlayer* rp) {
    if (!rp || rp->m_state.accountId != rp->id()) return nullptr;
    return &rp->m_state;
}

static void requestMusicSync() {
    g_musicSyncPending = true;
    g_musicResyncIn = MUSIC_RESYNC_DELAY;
}

// Moves the song to where it would be if we had played up to `pos` ourselves
static void syncMusicTo(PlayLayer* pl, CCPoint pos) {
    float time = pl->timeForPos(pos, 0, 0, false, 0) + pl->m_levelSettings->m_songOffset;
    if (time < 0.f) time = 0.f;
    FMODAudioEngine::get()->setMusicTimeMS(static_cast<unsigned int>(time * 1000.f), true, 0);
}

// Song follows the target: silent while they are dead, lined up with their position after
// anything that makes them jump (switching to them, a respawn, a practice checkpoint)
static void updateMusic(PlayLayer* pl, CCPoint pos, bool dead, float dt) {
    // platformer songs are not tied to a position
    if (pl->m_isPlatformer) return;

    auto fmod = FMODAudioEngine::get();
    if (dead) {
        if (fmod->isMusicPlaying(0)) fmod->pauseMusic(0);
        return;
    }
    if (g_wasDead) fmod->resumeMusic(0);

    if (g_musicSyncPending) {
        g_musicSyncPending = false;
        syncMusicTo(pl, pos);
    } else if (g_musicResyncIn > 0.f) {
        g_musicResyncIn -= dt;
        if (g_musicResyncIn <= 0.f) syncMusicTo(pl, pos);
    }
}

static void setCulling(bool enabled) {
    if (auto gl = globedLayer()) gl->toggleCullingEnabled(enabled);
}

static void setLocalVisible(PlayLayer* pl, bool visible) {
    for (auto p : {pl->m_player1, pl->m_player2}) {
        if (!p) continue;
        if (visible) {
            p->setVisible(p == pl->m_player1 || pl->m_gameState.m_isDualMode);
        } else {
            p->setVisible(false);
        }
        if (p->m_regularTrail) p->m_regularTrail->setVisible(visible);
        if (p->m_waveTrail) p->m_waveTrail->setVisible(visible);
        if (p->m_shipStreak) p->m_shipStreak->setVisible(visible);
    }
}

static void updateLabel(PlayLayer* pl) {
    auto ui = pl->m_uiLayer;
    if (!ui) return;

    auto label = static_cast<CCLabelBMFont*>(ui->getChildByID(LABEL_ID));
    if (!active()) {
        if (label) label->removeFromParent();
        return;
    }

    if (!label) {
        label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setID(LABEL_ID);
        label->setScale(.4f);
        label->setOpacity(160);
        label->setAnchorPoint({.5f, 1.f});
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({win.width / 2.f, win.height - 18.f});
        ui->addChild(label, 100);
    }
    label->setString(fmt::format("Spectating {}", g_targetName).c_str());
}

void stop(std::string_view reason = {});

void start(int id) {
    auto pl = PlayLayer::get();
    if (!pl || !sessionActive()) return;
    if (id == g_target) return;

    // switching is the same as stopping and picking someone new
    stop();

    // far away players are not sent to us otherwise
    setCulling(false);

    g_target = id;
    g_targetName = nameOf(id);
    g_hasLastPos = false;
    g_wasDead = false;
    requestMusicSync();
    updateLabel(pl);
}

// Forget everything without touching the level, for when the level is going away
void clear() {
    if (!active()) return;
    g_target = 0;
    g_hasLastPos = false;
    g_musicSyncPending = false;
    g_musicResyncIn = 0.f;
    setCulling(true);
    if (g_wasDead) FMODAudioEngine::get()->resumeMusic(0);
    g_wasDead = false;
}

void stop(std::string_view reason) {
    if (!active()) return;
    clear();

    if (!reason.empty()) {
        Notification::create(std::string(reason), NotificationIcon::Info)->show();
    }

    if (auto pl = PlayLayer::get()) {
        updateLabel(pl);
        setLocalVisible(pl, true);
        // our player was dragged through the level, never let that turn into a real attempt
        pl->resetLevel();
    }
}

void cycle(int dir) {
    auto ids = players();
    if (ids.empty()) return;

    auto it = std::find(ids.begin(), ids.end(), g_target);
    size_t idx = 0;
    if (it != ids.end()) {
        idx = (static_cast<size_t>(it - ids.begin()) + ids.size() + dir) % ids.size();
    } else if (dir < 0) {
        idx = ids.size() - 1;
    }
    start(ids[idx]);
}

// Runs before every game tick: puts our hidden player where the target is,
// so the camera, portals and triggers behave like they do for them.
void tick(PlayLayer* pl, float dt) {
    if (!active()) return;

    if (!sessionActive()) {
        stop("Spectating stopped: disconnected");
        return;
    }

    auto rp = remoteOf(g_target);
    auto node = rp ? reinterpret_cast<CCNode*>(rp->player1()) : nullptr;
    if (!node) {
        stop(fmt::format("{} left the level", g_targetName));
        return;
    }

    auto pos = node->getPosition();
    auto state = stateOf(rp.get());
    bool dead = state && state->isDead;

    bool respawned = g_wasDead && !dead;
    bool jumpedBack = g_hasLastPos && pos.x < g_lastPos.x - RESPAWN_JUMP;
    if (!pl->m_isPlatformer && (respawned || jumpedBack)) {
        // they respawned, restart our copy of the level so its triggers replay
        g_lastPos = pos;
        pl->resetLevel();
        requestMusicSync();
    }
    g_lastPos = pos;
    g_hasLastPos = true;

    updateMusic(pl, pos, dead, dt);
    g_wasDead = dead;

    auto p = pl->m_player1;
    p->m_position = pos;
    p->setPosition(pos);
    p->m_yVelocity = 0.0;

    setLocalVisible(pl, false);
}

} // namespace spectate

class SpectatePopup : public Popup {
protected:
    bool init() {
        if (!Popup::init(300.f, 230.f)) return false;
        this->setTitle("Spectate");

        auto ids = spectate::players();

        if (ids.empty()) {
            auto text = spectate::sessionActive()
                ? "Nobody else is in this level."
                : "You are not in a Globed session.";
            auto label = CCLabelBMFont::create(text, "bigFont.fnt");
            label->limitLabelWidth(250.f, .5f, .1f);
            m_mainLayer->addChildAtPosition(label, Anchor::Center);
        } else {
            constexpr float width = 260.f, rowHeight = 32.f, listHeight = 130.f;

            auto scroll = ScrollLayer::create({width, listHeight});
            auto content = scroll->m_contentLayer;
            float total = std::max(listHeight, rowHeight * ids.size());
            content->setContentSize({width, total});

            float y = total - rowHeight / 2.f;
            for (int id : ids) {
                auto menu = CCMenu::create();
                menu->setContentSize({width, rowHeight});
                menu->ignoreAnchorPointForPosition(false);
                menu->setPosition({width / 2.f, y});

                auto name = CCLabelBMFont::create(spectate::nameOf(id).c_str(), "bigFont.fnt");
                name->setAnchorPoint({0.f, .5f});
                name->limitLabelWidth(160.f, .55f, .1f);
                name->setPosition({8.f, rowHeight / 2.f});
                menu->addChild(name);

                bool current = spectate::target() == id;
                auto spr = ButtonSprite::create(
                    current ? "Watching" : "Watch", "goldFont.fnt",
                    current ? "GJ_button_02.png" : "GJ_button_01.png", .8f
                );
                spr->setScale(.6f);
                auto btn = CCMenuItemExt::createSpriteExtra(spr, [this, id](auto) {
                    spectate::start(id);
                    this->onClose(nullptr);
                });
                btn->setPosition({width - btn->getContentSize().width / 2.f - 8.f, rowHeight / 2.f});
                menu->addChild(btn);

                content->addChild(menu);
                y -= rowHeight;
            }

            scroll->ignoreAnchorPointForPosition(false);
            m_mainLayer->addChildAtPosition(scroll, Anchor::Center, {0.f, 5.f});
            scroll->scrollToTop();
        }

        if (spectate::active()) {
            auto spr = ButtonSprite::create("Stop spectating", "goldFont.fnt", "GJ_button_06.png", .8f);
            spr->setScale(.7f);
            auto btn = CCMenuItemExt::createSpriteExtra(spr, [this](auto) {
                spectate::stop();
                this->onClose(nullptr);
            });
            m_buttonMenu->addChildAtPosition(btn, Anchor::Bottom, {0.f, 24.f});
        }

        return true;
    }

public:
    static SpectatePopup* create() {
        auto ret = new SpectatePopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(SpectateGJBGL, GJBaseGameLayer) {
    void update(float dt) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            spectate::tick(pl, dt);
        }
        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (spectate::active() && PlayLayer::get()) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(SpectatePlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // the anticheat spike has to go through, the game uses it to check that dying works
        if (spectate::active() && object != m_anticheatSpike) return;
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (spectate::active()) return;
        PlayLayer::levelComplete();
    }

    void onQuit() {
        spectate::clear();
        PlayLayer::onQuit();
    }
};

class $modify(SpectatePauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = this->getChildByID("right-button-menu");
        if (!menu) {
            log::warn("right-button-menu not found, adding the spectate button in its own menu");
            menu = CCMenu::create();
            menu->setID("spectate-menu"_spr);
            auto win = CCDirector::get()->getWinSize();
            menu->setPosition({win.width - 40.f, win.height / 2.f});
            this->addChild(menu, 10);
        }

        CCSprite* spr = nullptr;
        if (CCSpriteFrameCache::get()->spriteFrameByName("gj_findBtn_001.png")) {
            spr = CircleButtonSprite::createWithSpriteFrameName(
                "gj_findBtn_001.png", 1.f, CircleBaseColor::Green, CircleBaseSize::Small
            );
        } else {
            spr = ButtonSprite::create("Spec", "goldFont.fnt", "GJ_button_01.png", .8f);
        }
        spr->setScale(.8f);

        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](auto) {
            SpectatePopup::create()->show();
        });
        btn->setID("spectate-button"_spr);
        menu->addChild(btn);
        menu->updateLayout();
    }
};

$on_mod(Loaded) {
    auto bind = [](const char* key, auto fn) {
        listenForKeybindSettingPresses(key, [fn](Keybind const&, bool down, bool repeat, double) {
            if (!down || repeat) return;
            auto pl = PlayLayer::get();
            if (!pl || pl->m_isPaused) return;
            if (!spectate::sessionActive()) {
                Notification::create("Not in a Globed session", NotificationIcon::Warning)->show();
                return;
            }
            fn();
        });
    };

    bind("key-next", [] { spectate::cycle(1); });
    bind("key-prev", [] { spectate::cycle(-1); });
    bind("key-stop", [] { spectate::stop(); });
}
