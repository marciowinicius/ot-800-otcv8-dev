/*
 * Copyright (c) 2010-2017 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "game.h"
#include "localplayer.h"
#include "map.h"
#include "tile.h"
#include "creature.h"
#include "container.h"
#include "statictext.h"
#include <framework/core/eventdispatcher.h>
#include <framework/ui/uimanager.h>
#include <framework/core/application.h>
#include <framework/core/resourcemanager.h>
#include "luavaluecasts_client.h"
#include "protocolgame.h"
#include "protocolcodes.h"

#include <framework/util/extras.h>
#include <framework/graphics/graph.h>
#include <framework/net/packet_player.h>
#include <framework/net/packet_recorder.h>
#include <framework/platform/platform.h>

Game g_game;

Game::Game()
{
    m_protocolVersion = 0;
    m_clientCustomOs = -1;
    m_clientVersion = 0;
    m_online = false;
    m_denyBotCall = false;
    m_dead = false;
    m_serverBeat = 50;
    m_seq = 0;
    m_ping = -1;
    m_pingDelay = 1000;
    m_newPingDelay = 250;
    m_canReportBugs = false;
    m_fightMode = Otc::FightBalanced;
    m_chaseMode = Otc::DontChase;
    m_pvpMode = Otc::WhiteDove;
    m_safeFight = true;
}

void Game::init()
{
    resetGameStates();
}

void Game::terminate()
{
    resetGameStates();
    m_protocolGame = nullptr;
}

void Game::resetGameStates()
{
    m_online = false;
    m_denyBotCall = false;
    m_dead = false;
    m_serverBeat = 50;
    m_seq = 0;
    m_ping = -1;
    m_canReportBugs = false;
    m_fightMode = Otc::FightBalanced;
    m_chaseMode = Otc::DontChase;
    m_pvpMode = Otc::WhiteDove;
    m_safeFight = true;
    m_followingCreature = nullptr;
    m_attackingCreature = nullptr;
    m_localPlayer = nullptr;
    m_pingSent = 0;
    m_pingReceived = 0;
    m_walkId = 0;
    m_walkPrediction = 0;
    m_coins = 0;
    m_transferableCoins = 0;
    m_newPingIds.clear();
    m_unjustifiedPoints = UnjustifiedPoints();

    for(auto& it : m_containers) {
        const ContainerPtr& container = it.second;
        if(container)
            container->onClose();
    }

    if(m_pingEvent) {
        m_pingEvent->cancel();
        m_pingEvent = nullptr;
    }

    if (m_newPingEvent) {
        m_newPingEvent->cancel();
        m_newPingEvent = nullptr;
    }

    if(m_checkConnectionEvent) {
        m_checkConnectionEvent->cancel();
        m_checkConnectionEvent = nullptr;
    }

    m_containers.clear();
    m_vips.clear();
    m_gmActions.clear();
    g_map.resetAwareRange();
}

void Game::processConnectionError(const boost::system::error_code& ec)
{
    // connection errors only have meaning if we still have a protocol
    if(m_protocolGame) {
        g_lua.callGlobalField("g_game", "onConnectionError", ec.message(), ec.value());
        processDisconnect();
    }
}

void Game::processDisconnect()
{
    if(isOnline())
        processGameEnd();

    if(m_protocolGame) {
        m_protocolGame->disconnect();
        m_protocolGame = nullptr;
    }
}

void Game::processUpdateNeeded(const std::string& signature)
{
    g_lua.callGlobalField("g_game", "onUpdateNeeded", signature);
}

void Game::processLoginError(const std::string& error)
{
    g_lua.callGlobalField("g_game", "onLoginError", error);
}

void Game::processLoginAdvice(const std::string& message)
{
    g_lua.callGlobalField("g_game", "onLoginAdvice", message);
}

void Game::processLoginWait(const std::string& message, int time)
{
    g_lua.callGlobalField("g_game", "onLoginWait", message, time);
}

void Game::processLoginToken(bool unknown)
{
    g_lua.callGlobalField("g_game", "onLoginToken", unknown);
}

void Game::processLogin()
{
    g_lua.callGlobalField("g_game", "onLogin");
}

void Game::processPendingGame()
{
    m_localPlayer->setPendingGame(true);
    g_lua.callGlobalField("g_game", "onPendingGame");
    m_protocolGame->sendEnterGame();
}

void Game::processEnterGame()
{
    m_localPlayer->setPendingGame(false);
    g_lua.callGlobalField("g_game", "onEnterGame");
}

void Game::processGameStart()
{
    m_online = true;

    // synchronize fight modes with the server
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight, m_pvpMode);

    // NOTE: the entire map description and local player information is not known yet (bot call is allowed here)
    enableBotCall();
    g_lua.callGlobalField("g_game", "onGameStart");
    disableBotCall();

    if (g_game.getFeature(Otc::GameExtendedClientPing)) {
        m_newPingEvent = g_dispatcher.scheduleEvent([] {
            g_game.newPing();
        }, m_newPingDelay);
    }
    if(g_game.getFeature(Otc::GameClientPing)) {
        m_pingEvent = g_dispatcher.scheduleEvent([] {
            g_game.ping();
        }, m_pingDelay);
    }

    m_checkConnectionEvent = g_dispatcher.cycleEvent([this] {
        if(!g_game.isConnectionOk() && !m_connectionFailWarned) {
            g_lua.callGlobalField("g_game", "onConnectionFailing", true);
            m_connectionFailWarned = true;
        } else if(g_game.isConnectionOk() && m_connectionFailWarned) {
            g_lua.callGlobalField("g_game", "onConnectionFailing", false);
            m_connectionFailWarned = false;
        }
    }, 1000);
}

void Game::processGameEnd()
{
    m_online = false;
    g_lua.callGlobalField("g_game", "onGameEnd");

    if(m_connectionFailWarned) {
        g_lua.callGlobalField("g_game", "onConnectionFailing", false);
        m_connectionFailWarned = false;
    }

    // reset game state
    resetGameStates();

    m_worldName = "";
    m_characterName = "";

    // clean map creatures
    g_map.cleanDynamicThings();
}

void Game::processDeath(int deathType, int penality)
{
    m_dead = true;
    m_localPlayer->stopWalk();

    g_lua.callGlobalField("g_game", "onDeath", deathType, penality);
}

void Game::processGMActions(const std::vector<uint8>& actions)
{
    m_gmActions = actions;
    g_lua.callGlobalField("g_game", "onGMActions", actions);
}

void Game::processPlayerHelpers(int helpers)
{
    g_lua.callGlobalField("g_game", "onPlayerHelpersUpdate", helpers);
}

void Game::processPlayerModes(Otc::FightModes fightMode, Otc::ChaseModes chaseMode, bool safeMode, Otc::PVPModes pvpMode)
{
    m_fightMode = fightMode;
    m_chaseMode = chaseMode;
    m_safeFight = safeMode;
    m_pvpMode = pvpMode;

    g_lua.callGlobalField("g_game", "onFightModeChange", fightMode);
    g_lua.callGlobalField("g_game", "onChaseModeChange", chaseMode);
    g_lua.callGlobalField("g_game", "onSafeFightChange", safeMode);
    g_lua.callGlobalField("g_game", "onPVPModeChange", pvpMode);
}

void Game::processPing()
{
    g_lua.callGlobalField("g_game", "onPing");
    enableBotCall();
    m_protocolGame->sendPingBack();
    disableBotCall();
}

void Game::processPingBack()
{
    m_pingReceived++;

    if (!g_game.getFeature(Otc::GameExtendedClientPing)) {
        if (m_pingReceived == m_pingSent) {
            m_ping = m_pingTimer.elapsed_millis();
            g_graphs[GRAPH_LATENCY].addValue(m_ping);
        }

        g_lua.callGlobalField("g_game", "onPingBack", m_ping);
    }

    m_pingEvent = g_dispatcher.scheduleEvent([] {
        g_game.ping();
    }, m_pingDelay);
}

void Game::processNewPing(uint32_t pingId)
{
    auto it = m_newPingIds.find(pingId);

    if (it == m_newPingIds.end())
        return;

    m_ping = it->second.elapsed_millis();
    g_graphs[GRAPH_LATENCY].addValue(m_ping);
    g_lua.callGlobalField("g_game", "onPingBack", m_ping);
}

void Game::processTextMessage(Otc::MessageMode mode, const std::string& text)
{
    g_lua.callGlobalField("g_game", "onTextMessage", mode, text);
}

void Game::processTalk(const std::string& name, int level, Otc::MessageMode mode, const std::string& text, int channelId, const Position& pos)
{
    g_lua.callGlobalField("g_game", "onTalk", name, level, mode, text, channelId, pos);
}

void Game::processOpenContainer(int containerId, const ItemPtr& containerItem, const std::string& name, int capacity, bool hasParent, const std::vector<ItemPtr>& items, bool isUnlocked, bool hasPages, int containerSize, int firstIndex)
{
    ContainerPtr previousContainer = getContainer(containerId);
    ContainerPtr container = ContainerPtr(new Container(containerId, capacity, name, containerItem, hasParent, isUnlocked, hasPages, containerSize, firstIndex));
    m_containers[containerId] = container;
    container->onAddItems(items);

    // we might want to close a container here
    enableBotCall();
    container->onOpen(previousContainer);
    disableBotCall();

    if(previousContainer)
        previousContainer->onClose();
}

void Game::processCloseContainer(int containerId)
{
    ContainerPtr container = getContainer(containerId);
    if(!container) {
        return;
    }

    m_containers[containerId] = nullptr;
    container->onClose();
}

void Game::processContainerAddItem(int containerId, const ItemPtr& item, int slot)
{
    ContainerPtr container = getContainer(containerId);
    if(!container) {
        return;
    }

    container->onAddItem(item, slot);
}

void Game::processContainerUpdateItem(int containerId, int slot, const ItemPtr& item)
{
    ContainerPtr container = getContainer(containerId);
    if(!container) {
        return;
    }

    container->onUpdateItem(slot, item);
}

void Game::processContainerRemoveItem(int containerId, int slot, const ItemPtr& lastItem)
{
    ContainerPtr container = getContainer(containerId);
    if(!container) {
        return;
    }

    container->onRemoveItem(slot, lastItem);
}

void Game::processInventoryChange(int slot, const ItemPtr& item)
{
    if(item)
        item->setPosition(Position(65535, slot, 0));

    m_localPlayer->setInventoryItem((Otc::InventorySlot)slot, item);
}

void Game::processChannelList(const std::vector<std::tuple<int, std::string> >& channelList)
{
    g_lua.callGlobalField("g_game", "onChannelList", channelList);
}

void Game::processOpenChannel(int channelId, const std::string& name)
{
    g_lua.callGlobalField("g_game", "onOpenChannel", channelId, name);
}

void Game::processOpenPrivateChannel(const std::string& name)
{
    g_lua.callGlobalField("g_game", "onOpenPrivateChannel", name);
}

void Game::processOpenOwnPrivateChannel(int channelId, const std::string& name)
{
    g_lua.callGlobalField("g_game", "onOpenOwnPrivateChannel", channelId, name);
}

void Game::processCloseChannel(int channelId)
{
    g_lua.callGlobalField("g_game", "onCloseChannel", channelId);
}

void Game::processRuleViolationChannel(int channelId)
{
    g_lua.callGlobalField("g_game", "onRuleViolationChannel", channelId);
}

void Game::processRuleViolationRemove(const std::string& name)
{
    g_lua.callGlobalField("g_game", "onRuleViolationRemove", name);
}

void Game::processRuleViolationCancel(const std::string& name)
{
    g_lua.callGlobalField("g_game", "onRuleViolationCancel", name);
}

void Game::processRuleViolationLock()
{
    g_lua.callGlobalField("g_game", "onRuleViolationLock");
}

void Game::processVipAdd(uint id, const std::string& name, uint status, const std::string& description, int iconId, bool notifyLogin)
{
    m_vips[id] = Vip(name, status, description, iconId, notifyLogin);
    g_lua.callGlobalField("g_game", "onAddVip", id, name, status, description, iconId, notifyLogin);
}

void Game::processVipStateChange(uint id, uint status)
{
    if (m_vips.find(id) == m_vips.end()) return;
    std::get<1>(m_vips[id]) = status;
    g_lua.callGlobalField("g_game", "onVipStateChange", id, status);
}

void Game::processTutorialHint(int id)
{
    g_lua.callGlobalField("g_game", "onTutorialHint", id);
}

void Game::processAddAutomapFlag(const Position& pos, int icon, const std::string& message)
{
    g_lua.callGlobalField("g_game", "onAddAutomapFlag", pos, icon, message);
}

void Game::processRemoveAutomapFlag(const Position& pos, int icon, const std::string& message)
{
    g_lua.callGlobalField("g_game", "onRemoveAutomapFlag", pos, icon, message);
}

void Game::processOpenOutfitWindow(const Outfit& currentOutfit, const std::vector<std::tuple<int, std::string, int>>& outfitList,
                                   const std::vector<std::tuple<int, std::string>>& mountList,
                                   const std::vector<std::tuple<int, std::string>>& wingList,
                                   const std::vector<std::tuple<int, std::string>>& auraList,
                                   const std::vector<std::tuple<int, std::string>>& shaderList,
                                   const std::vector<std::tuple<int, std::string>>& healthBarList,
                                   const std::vector<std::tuple<int, std::string>>& manaBarList)
{
    g_lua.callGlobalField("g_game", "onOpenOutfitWindow", currentOutfit, outfitList, mountList, wingList, auraList, shaderList, healthBarList, manaBarList);
}

void Game::processOpenNpcTrade(const std::vector<std::tuple<ItemPtr, std::string, int, int64_t, int64_t> >& items)
{
    g_lua.callGlobalField("g_game", "onOpenNpcTrade", items);
}

void Game::processPlayerGoods(uint64_t money, const std::vector<std::tuple<ItemPtr, int> >& goods)
{
    g_lua.callGlobalField("g_game", "onPlayerGoods", money, goods);
}

void Game::processCloseNpcTrade()
{
    g_lua.callGlobalField("g_game", "onCloseNpcTrade");
}

void Game::processOwnTrade(const std::string& name, const std::vector<ItemPtr>& items)
{
    g_lua.callGlobalField("g_game", "onOwnTrade", name, items);
}

void Game::processCounterTrade(const std::string& name, const std::vector<ItemPtr>& items)
{
    g_lua.callGlobalField("g_game", "onCounterTrade", name, items);
}

void Game::processCloseTrade()
{
    g_lua.callGlobalField("g_game", "onCloseTrade");
}

void Game::processEditText(uint id, int itemId, int maxLength, const std::string& text, const std::string& writer, const std::string& date)
{
    g_lua.callGlobalField("g_game", "onEditText", id, itemId, maxLength, text, writer, date);
}

void Game::processEditList(uint id, int doorId, const std::string& text)
{
    g_lua.callGlobalField("g_game", "onEditList", id, doorId, text);
}

void Game::processQuestLog(const std::vector<std::tuple<int, std::string, bool> >& questList)
{
    g_lua.callGlobalField("g_game", "onQuestLog", questList);
}

void Game::processQuestLine(int questId, const std::vector<std::tuple<std::string, std::string, int> >& questMissions)
{
    g_lua.callGlobalField("g_game", "onQuestLine", questId, questMissions);
}

void Game::processModalDialog(uint32 id, std::string title, std::string message, std::vector<std::tuple<int, std::string> > buttonList, int enterButton, int escapeButton, std::vector<std::tuple<int, std::string> > choiceList, bool priority)
{
    g_lua.callGlobalField("g_game", "onModalDialog", id, title, message, buttonList, enterButton, escapeButton, choiceList, priority);
}

void Game::processAttackCancel(uint seq)
{
    if(seq == 0 || m_seq == seq)
        cancelAttack();
}

void Game::processWalkCancel(Otc::Direction direction)
{
    m_localPlayer->cancelWalk(direction);
}

void Game::processNewWalkCancel(Otc::Direction dir) 
{
    m_walkId += 1;
    m_localPlayer->cancelNewWalk(dir);
}

void Game::processPredictiveWalkCancel(const Position& pos, Otc::Direction dir)
{
    m_walkPrediction += 1;
    if (m_localPlayer->predictiveCancelWalk(pos, m_walkPrediction, dir)) {
        m_walkId += 1;
    }
}

void Game::processWalkId(uint32_t walkId)
{
    m_walkId = std::max(m_walkId, walkId); // fixes desync
}

void Game::loginWorld(const std::string& account, const std::string& password, const std::string& worldName, const std::string& worldHost, int worldPort, const std::string& characterName, const std::string& authenticatorToken, const std::string& sessionKey, const std::string& recordTo)
{
    if(m_protocolGame || isOnline())
        stdext::throw_exception("Unable to login into a world while already online or logging.");

    if(m_protocolVersion == 0)
        stdext::throw_exception("Must set a valid game protocol version before logging.");

    // reset the new game state
    resetGameStates();

    m_localPlayer = LocalPlayerPtr(new LocalPlayer);
    m_localPlayer->setName(characterName);

    m_protocolGame = ProtocolGamePtr(new ProtocolGame);
    if (!recordTo.empty()) {
        m_protocolGame->setRecorder(PacketRecorderPtr(new PacketRecorder(recordTo)));
    }
    m_protocolGame->login(account, password, worldHost, (uint16)worldPort, characterName, authenticatorToken, sessionKey, worldName);
    m_characterName = characterName;
    m_worldName = worldName;
}

void Game::playRecord(const std::string& file)
{
    if (m_protocolGame || isOnline())
        stdext::throw_exception("Unable to login into a world while already online or logging.");

    if (m_protocolVersion == 0)
        stdext::throw_exception("Must set a valid game protocol version before logging.");

    auto packetPlayer = PacketPlayerPtr(new PacketPlayer(file));
    if (!packetPlayer)
        stdext::throw_exception("Invalid record file.");

    // reset the new game state
    resetGameStates();

    m_localPlayer = LocalPlayerPtr(new LocalPlayer);
    m_localPlayer->setName("Player");

    m_protocolGame = ProtocolGamePtr(new ProtocolGame);
    m_protocolGame->playRecord(packetPlayer);
    m_characterName = "Player";
    m_worldName = "Record";
}

void Game::cancelLogin()
{
    // send logout even if the game has not started yet, to make sure that the player doesn't stay logged there
    if(m_protocolGame)
        m_protocolGame->sendLogout();

    g_lua.callGlobalField("g_game", "onLogout");
    processDisconnect();
}

void Game::forceLogout()
{
    if(!isOnline())
        return;

    g_lua.callGlobalField("g_game", "onLogout");
    m_protocolGame->sendLogout();
    processDisconnect();
}

void Game::safeLogout()
{
    if(!isOnline())
        return;

    g_lua.callGlobalField("g_game", "onLogout");
    m_protocolGame->sendLogout();
}

void Game::autoWalk(const std::vector<Otc::Direction>& dirs, Position startPos)
{
    if(!canPerformGameAction())
        return;

    if (dirs.size() == 0)
        return;

    // protocol limits walk path
    if((!g_game.getFeature(Otc::GameNewWalking) || dirs.size() > 4097) && dirs.size() > 127) {
        g_logger.error("Auto walk path too great");
        return;
    }

    if (g_extras.debugWalking) {
        g_logger.info(stdext::format("[%i] Game::autoWalk", (int)g_clock.millis()));
    }

    // must cancel follow before any new walk
    if (isFollowing()) {
        cancelFollow();
    }

    auto it = dirs.begin();
    Otc::Direction direction = *it;

    uint8_t flags = 0x04; // auto walk flag

    TilePtr toTile = g_map.getTile(startPos.translatedToDirection(direction));
    if(startPos == m_localPlayer->getPrewalkingPosition() && toTile && toTile->isWalkable() && !m_localPlayer->isWalking() && m_localPlayer->canWalk(direction, true)) {
        m_localPlayer->preWalk(direction);
        m_localPlayer->startServerWalking();
        flags |= 0x01; // prewalk flag
    }

    g_lua.callGlobalField("g_game", "onAutoWalk", dirs);

    if (g_game.getFeature(Otc::GameNewWalking))
        m_protocolGame->sendNewWalk(m_walkId, m_walkPrediction, startPos, flags, dirs);
    else
        m_protocolGame->sendAutoWalk(dirs);
}

void Game::walk(Otc::Direction direction, bool withPreWalk)
{
    m_denyBotCall = false;
    if (!canPerformGameAction()) {
        m_denyBotCall = true;
        return;
    }
    if (g_extras.debugWalking) {
        g_logger.info(stdext::format("[%i] Game::walk", (int)g_clock.millis()));
    }

    g_lua.callGlobalField("g_game", "onWalk", direction, withPreWalk);

    if (g_game.getFeature(Otc::GameNewWalking)) {
        Position pos = m_localPlayer->getPrewalkingPosition();
        uint8_t flags = 0;
        if (withPreWalk) {
            flags |= 0x01;
        }
        m_protocolGame->sendNewWalk(m_walkId, m_walkPrediction, pos, flags, { direction });
        m_denyBotCall = true;
        return;
    }

    switch(direction) {
    case Otc::North:
        m_protocolGame->sendWalkNorth();
        break;
    case Otc::East:
        m_protocolGame->sendWalkEast();
        break;
    case Otc::South:
        m_protocolGame->sendWalkSouth();
        break;
    case Otc::West:
        m_protocolGame->sendWalkWest();
        break;
    case Otc::NorthEast:
        m_protocolGame->sendWalkNorthEast();
        break;
    case Otc::SouthEast:
        m_protocolGame->sendWalkSouthEast();
        break;
    case Otc::SouthWest:
        m_protocolGame->sendWalkSouthWest();
        break;
    case Otc::NorthWest:
        m_protocolGame->sendWalkNorthWest();
        break;
    default:
        break;
    }
    m_denyBotCall = true;
}

void Game::turn(Otc::Direction direction)
{
    m_denyBotCall = false;
    if (!canPerformGameAction()) {
        m_denyBotCall = true;
        return;
    }

    if (g_game.getFeature(Otc::GameNewWalking)) {
        m_localPlayer->setDirection(direction);
    }

    switch(direction) {
    case Otc::North:
        m_protocolGame->sendTurnNorth();
        break;
    case Otc::East:
        m_protocolGame->sendTurnEast();
        break;
    case Otc::South:
        m_protocolGame->sendTurnSouth();
        break;
    case Otc::West:
        m_protocolGame->sendTurnWest();
        break;
    default:
        break;
    }
    m_denyBotCall = true;
}

void Game::stop()
{
    if (g_extras.debugWalking) {
        g_logger.info(stdext::format("[%i] Game::stop", (int)g_clock.millis()));
    }

    m_denyBotCall = false;
    if (!canPerformGameAction()) {
        m_denyBotCall = true;
        return;
    }

    if(isFollowing())
        cancelFollow(); // can change m_denyBotCall
    m_denyBotCall = false;

    m_protocolGame->sendStop();
    m_denyBotCall = true;
}

void Game::look(const ThingPtr& thing, bool isBattleList)
{
    if(!canPerformGameAction() || !thing)
        return;

    if(thing->isCreature() && isBattleList && m_protocolVersion >= 961)
        m_protocolGame->sendLookCreature(thing->getId());
    else
        m_protocolGame->sendLook(thing->getPosition(), thing->getId(), thing->getStackPos());
}

void Game::move(const ThingPtr& thing, const Position& toPos, int count)
{
    if (count <= 0)
        count = 1;

    if (!canPerformGameAction() || !thing || thing->getPosition() == toPos)
        return;

    uint id = thing->getId();
    if (thing->isCreature()) {
        CreaturePtr creature = thing->static_self_cast<Creature>();
        id = Proto::Creature;
    }

    m_protocolGame->sendMove(thing->getPosition(), id, thing->getStackPos(), toPos, count);
}

void Game::moveRaw(const Position& pos, int id, int stackpos, const Position& toPos, int count)
{
    if (!canPerformGameAction())
        return;

    m_protocolGame->sendMove(pos, id, stackpos, toPos, count);
}

void Game::moveToParentContainer(const ThingPtr& thing, int count)
{
    if(!canPerformGameAction() || !thing || count <= 0)
        return;

    Position position = thing->getPosition();
    move(thing, Position(position.x, position.y, 254), count);
}

void Game::rotate(const ThingPtr& thing)
{
    if(!canPerformGameAction() || !thing)
        return;

    m_protocolGame->sendRotateItem(thing->getPosition(), thing->getId(), thing->getStackPos());
}

void Game::wrap(const ThingPtr& thing)
{
    if (!canPerformGameAction() || !thing)
        return;

    m_protocolGame->sendWrapableItem(thing->getPosition(), thing->getId(), thing->getStackPos());
}

void Game::use(const ThingPtr& thing)
{
    if(!canPerformGameAction() || !thing)
        return;

    Position pos = thing->getPosition();
    if(!pos.isValid()) // virtual item
        pos = Position(0xFFFF, 0, 0); // inventory item

    // some items, e.g. parcel, are not set as containers but they are.
    // always try to use these items in free container slots.
    m_protocolGame->sendUseItem(pos, thing->getId(), thing->getStackPos(), findEmptyContainerId());

    g_lua.callGlobalField("g_game", "onUse", pos, thing->getId(), thing->getStackPos(), 0);
}

void Game::useInventoryItem(int itemId, int subType)
{
    if(!canPerformGameAction() || !g_things.isValidDatId(itemId, ThingCategoryItem))
        return;

    Position pos = Position(0xFFFF, 0, 0); // means that is a item in inventory

    m_protocolGame->sendUseItem(pos, itemId, 0, subType);

    g_lua.callGlobalField("g_game", "onUse", pos, itemId, 0, subType);
}

void Game::useWith(const ItemPtr& item, const ThingPtr& toThing, int subType)
{
    if(!canPerformGameAction() || !item || !toThing)
        return;

    Position pos = item->getPosition();
    if(!pos.isValid()) // virtual item
        pos = Position(0xFFFF, 0, 0); // means that is an item in inventory

    if(toThing->isCreature() && (g_game.getProtocolVersion() >= 780 || g_game.getFeature(Otc::GameForceAllowItemHotkeys)))
        m_protocolGame->sendUseOnCreature(pos, item->getId(), subType ? subType : item->getStackPos(), toThing->getId());
    else
        m_protocolGame->sendUseItemWith(pos, item->getId(), subType ? subType : item->getStackPos(), toThing->getPosition(), toThing->getId(), toThing->getStackPos());

    g_lua.callGlobalField("g_game", "onUseWith", pos, item->getId(), toThing, subType);
}

void Game::useInventoryItemWith(int itemId, const ThingPtr& toThing, int subType)
{
    if(!canPerformGameAction() || !toThing)
        return;

    Position pos = Position(0xFFFF, 0, 0); // means that is a item in inventory

    if(toThing->isCreature())
        m_protocolGame->sendUseOnCreature(pos, itemId, subType, toThing->getId());
    else
        m_protocolGame->sendUseItemWith(pos, itemId, subType, toThing->getPosition(), toThing->getId(), toThing->getStackPos());

    g_lua.callGlobalField("g_game", "onUseWith", pos, itemId, toThing, subType);
}

ItemPtr Game::findItemInContainers(uint itemId, int subType)
{
    for(auto& it : m_containers) {
        const ContainerPtr& container = it.second;

        if(container) {
            ItemPtr item = container->findItemById(itemId, subType);
            if(item != nullptr)
                return item;
        }
    }
    return nullptr;
}

int Game::open(const ItemPtr& item, const ContainerPtr& previousContainer)
{
    if(!canPerformGameAction() || !item)
        return -1;

    int id = 0;
    if(!previousContainer)
        id = findEmptyContainerId();
    else
        id = previousContainer->getId();

    m_protocolGame->sendUseItem(item->getPosition(), item->getId(), item->getStackPos(), id);
    return id;
}

void Game::openParent(const ContainerPtr& container)
{
    if(!canPerformGameAction() || !container)
        return;

    m_protocolGame->sendUpContainer(container->getId());
}

void Game::close(const ContainerPtr& container)
{
    if(!canPerformGameAction() || !container)
        return;

    m_protocolGame->sendCloseContainer(container->getId());
}

void Game::refreshContainer(const ContainerPtr& container)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRefreshContainer(container->getId());
}

void Game::attack(CreaturePtr creature, bool cancel)
{
    if(!canPerformGameAction() || creature == m_localPlayer)
        return;

    // cancel when attacking again
    if(creature && creature == m_attackingCreature)
        creature = nullptr;

    if(creature && isFollowing())
        cancelFollow();

    setAttackingCreature(creature);
    m_localPlayer->stopAutoWalk();

    if(m_protocolVersion >= 963) {
        if(creature)
            m_seq = creature->getId();
    } else
        m_seq++;

    if(!cancel)
        m_protocolGame->sendAttack(creature ? creature->getId() : 0, m_seq);
}

void Game::follow(CreaturePtr creature)
{
    m_denyBotCall = false;
    if (!canPerformGameAction() || creature == m_localPlayer) {
        m_denyBotCall = true;
        return;
    }

    // cancel when following again
    if(creature && creature == m_followingCreature)
        creature = nullptr;

    if(creature && isAttacking())
        cancelAttack();

    setFollowingCreature(creature);
    m_localPlayer->stopAutoWalk();

    if(m_protocolVersion >= 963) {
        if(creature)
            m_seq = creature->getId();
    } else
        m_seq++;

    m_protocolGame->sendFollow(creature ? creature->getId() : 0, m_seq);
    m_denyBotCall = true;
}

void Game::cancelAttackAndFollow()
{
    if(!canPerformGameAction())
        return;

    if(isFollowing())
        setFollowingCreature(nullptr);
    if(isAttacking())
        setAttackingCreature(nullptr);

    m_localPlayer->stopAutoWalk();

    m_protocolGame->sendCancelAttackAndFollow();

    g_lua.callGlobalField("g_game", "onCancelAttackAndFollow");
}

void Game::talk(const std::string& message)
{
    if(!canPerformGameAction() || message.empty())
        return;

    talkChannel(Otc::MessageSay, 0, message);
}

void Game::talkChannel(Otc::MessageMode mode, int channelId, const std::string& message)
{
    if(!canPerformGameAction() || message.empty())
        return;
        
    m_protocolGame->sendTalk(mode, channelId, "", message, m_localPlayer->getPosition(), m_localPlayer->getDirection());
}

void Game::talkPrivate(Otc::MessageMode mode, const std::string& receiver, const std::string& message)
{
    if(!canPerformGameAction() || receiver.empty() || message.empty())
        return;
    m_protocolGame->sendTalk(mode, 0, receiver, message, m_localPlayer->getPosition(), m_localPlayer->getDirection());
}

void Game::openPrivateChannel(const std::string& receiver)
{
    if(!canPerformGameAction() || receiver.empty())
        return;
    m_protocolGame->sendOpenPrivateChannel(receiver);
}

void Game::requestChannels()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestChannels();
}

void Game::joinChannel(int channelId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendJoinChannel(channelId);
}

void Game::leaveChannel(int channelId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendLeaveChannel(channelId);
}

void Game::closeNpcChannel()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendCloseNpcChannel();
}

void Game::openOwnChannel()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendOpenOwnChannel();
}

void Game::inviteToOwnChannel(const std::string& name)
{
    if(!canPerformGameAction() || name.empty())
        return;
    m_protocolGame->sendInviteToOwnChannel(name);
}

void Game::excludeFromOwnChannel(const std::string& name)
{
    if(!canPerformGameAction() || name.empty())
        return;
    m_protocolGame->sendExcludeFromOwnChannel(name);
}

void Game::partyInvite(int creatureId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendInviteToParty(creatureId);
}

void Game::partyJoin(int creatureId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendJoinParty(creatureId);
}

void Game::partyRevokeInvitation(int creatureId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRevokeInvitation(creatureId);
}

void Game::partyPassLeadership(int creatureId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendPassLeadership(creatureId);
}

void Game::partyLeave()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendLeaveParty();
}

void Game::partyShareExperience(bool active)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendShareExperience(active);
}

void Game::requestOutfit()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestOutfit();
}

void Game::changeOutfit(const Outfit& outfit)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendChangeOutfit(outfit);
}

void Game::addVip(const std::string& name)
{
    if(!canPerformGameAction() || name.empty())
        return;
    m_protocolGame->sendAddVip(name);
}

void Game::removeVip(int playerId)
{
    if(!canPerformGameAction())
        return;

    auto it = m_vips.find(playerId);
    if(it == m_vips.end())
        return;
    m_vips.erase(it);
    m_protocolGame->sendRemoveVip(playerId);
}

void Game::editVip(int playerId, const std::string& description, int iconId, bool notifyLogin)
{
    if(!canPerformGameAction())
        return;

    auto it = m_vips.find(playerId);
    if(it == m_vips.end())
        return;

    std::get<2>(m_vips[playerId]) = description;
    std::get<3>(m_vips[playerId]) = iconId;
    std::get<4>(m_vips[playerId]) = notifyLogin;

    if(getFeature(Otc::GameAdditionalVipInfo))
        m_protocolGame->sendEditVip(playerId, description, iconId, notifyLogin);
}

void Game::setChaseMode(Otc::ChaseModes chaseMode)
{
    if(!canPerformGameAction())
        return;
    if(m_chaseMode == chaseMode)
        return;
    m_chaseMode = chaseMode;
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight, m_pvpMode);
    g_lua.callGlobalField("g_game", "onChaseModeChange", chaseMode);
}

void Game::setFightMode(Otc::FightModes fightMode)
{
    if(!canPerformGameAction())
        return;
    if(m_fightMode == fightMode)
        return;
    m_fightMode = fightMode;
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight, m_pvpMode);
    g_lua.callGlobalField("g_game", "onFightModeChange", fightMode);
}

void Game::setSafeFight(bool on)
{
    if(!canPerformGameAction())
        return;
    if(m_safeFight == on)
        return;
    m_safeFight = on;
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight, m_pvpMode);
    g_lua.callGlobalField("g_game", "onSafeFightChange", on);
}

void Game::setPVPMode(Otc::PVPModes pvpMode)
{
    if(!canPerformGameAction())
        return;
    if(!getFeature(Otc::GamePVPMode))
        return;
    if(m_pvpMode == pvpMode)
        return;
    m_pvpMode = pvpMode;
    m_protocolGame->sendChangeFightModes(m_fightMode, m_chaseMode, m_safeFight, m_pvpMode);
    g_lua.callGlobalField("g_game", "onPVPModeChange", pvpMode);
}

void Game::setUnjustifiedPoints(UnjustifiedPoints unjustifiedPoints)
{
    if(!canPerformGameAction())
        return;
    if(!getFeature(Otc::GameUnjustifiedPoints))
        return;
    if(m_unjustifiedPoints == unjustifiedPoints)
        return;

    m_unjustifiedPoints = unjustifiedPoints;
    g_lua.callGlobalField("g_game", "onUnjustifiedPointsChange", unjustifiedPoints);
}

void Game::setOpenPvpSituations(int openPvpSituations)
{
    if(!canPerformGameAction())
        return;
    if(m_openPvpSituations == openPvpSituations)
        return;

    m_openPvpSituations = openPvpSituations;
    g_lua.callGlobalField("g_game", "onOpenPvpSituationsChange", openPvpSituations);
}


void Game::inspectNpcTrade(const ItemPtr& item)
{
    if(!canPerformGameAction() || !item)
        return;
    m_protocolGame->sendInspectNpcTrade(item->getId(), item->getCount());
}

void Game::buyItem(const ItemPtr& item, int amount, bool ignoreCapacity, bool buyWithBackpack)
{
    if(!canPerformGameAction() || !item)
        return;
    m_protocolGame->sendBuyItem(item->getId(), item->getCountOrSubType(), amount, ignoreCapacity, buyWithBackpack);
}

void Game::sellItem(const ItemPtr& item, int amount, bool ignoreEquipped)
{
    if(!canPerformGameAction() || !item)
        return;
    m_protocolGame->sendSellItem(item->getId(), item->getCountOrSubType(), amount, ignoreEquipped);
}

void Game::closeNpcTrade()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendCloseNpcTrade();
}

void Game::requestTrade(const ItemPtr& item, const CreaturePtr& creature)
{
    if(!canPerformGameAction() || !item || !creature)
        return;
    m_protocolGame->sendRequestTrade(item->getPosition(), item->getId(), item->getStackPos(), creature->getId());
}

void Game::inspectTrade(bool counterOffer, int index)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendInspectTrade(counterOffer, index);
}

void Game::acceptTrade()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendAcceptTrade();
}

void Game::rejectTrade()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRejectTrade();
}

void Game::editText(uint id, const std::string& text)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendEditText(id, text);
}

void Game::editList(uint id, int doorId, const std::string& text)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendEditList(id, doorId, text);
}

void Game::openRuleViolation(const std::string& reporter)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendOpenRuleViolation(reporter);
}

void Game::closeRuleViolation(const std::string& reporter)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendCloseRuleViolation(reporter);
}

void Game::cancelRuleViolation()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendCancelRuleViolation();
}

void Game::reportBug(const std::string& comment)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendBugReport(comment);
}

void Game::reportRuleViolation(const std::string& target, int reason, int action, const std::string& comment, const std::string& statement, int statementId, bool ipBanishment)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRuleViolation(target, reason, action, comment, statement, statementId, ipBanishment);
}

void Game::debugReport(const std::string& a, const std::string& b, const std::string& c, const std::string& d)
{
    m_protocolGame->sendDebugReport(a, b, c, d);
}

void Game::requestQuestLog()
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestQuestLog();
}

void Game::requestQuestLine(int questId)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestQuestLine(questId);
}

void Game::equipItem(const ItemPtr& item)
{
    if (!item || !canPerformGameAction())
        return;
    m_protocolGame->sendEquipItem(item->getId(), item->getCountOrSubType());
}

void Game::equipItemId(int itemId, int subType)
{
    if (!canPerformGameAction())
        return;
    m_protocolGame->sendEquipItem(itemId, subType);
}

void Game::mount(bool mount)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendOutfitExtensionStatus(mount ? 1 : 0);
}

void Game::setOutfitExtensions(int mount, int wings, int aura, int shader, int healthBar, int manaBar)
{
    if (!canPerformGameAction())
        return;
    m_protocolGame->sendOutfitExtensionStatus(mount, wings, aura, shader, healthBar, manaBar);
}

void Game::requestItemInfo(const ItemPtr& item, int index)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestItemInfo(item->getId(), item->getSubType(), index);
}

void Game::answerModalDialog(uint32 dialog, int button, int choice)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendAnswerModalDialog(dialog, button, choice);
}

void Game::browseField(const Position& position)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendBrowseField(position);
}

void Game::seekInContainer(int cid, int index)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendSeekInContainer(cid, index);
}

void Game::buyStoreOffer(int offerId, int productType, const std::string& name)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendBuyStoreOffer(offerId, productType, name);
}

void Game::requestTransactionHistory(int page, int entriesPerPage)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestTransactionHistory(page, entriesPerPage);
}

void Game::requestStoreOffers(const std::string& categoryName, int serviceType)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendRequestStoreOffers(categoryName, serviceType);
}

void Game::openStore(int serviceType)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendOpenStore(serviceType);
}

void Game::transferCoins(const std::string& recipient, int amount)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendTransferCoins(recipient, amount);
}

void Game::openTransactionHistory(int entriesPerPage)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendOpenTransactionHistory(entriesPerPage);
}

void Game::preyAction(int slot, int actionType, int index)
{
    if (!canPerformGameAction())
        return;
    m_protocolGame->sendPreyAction(slot, actionType, index);
}

void Game::preyRequest()
{
    if (!canPerformGameAction())
        return;
    m_protocolGame->sendPreyRequest();
}


void Game::applyImbuement(uint8_t slot, uint32_t imbuementId, bool protectionCharm)
{
    if (!canPerformGameAction())
        return;
    m_protocolGame->sendApplyImbuement(slot, imbuementId, protectionCharm);
}

void Game::clearImbuement(uint8_t slot)
{
    if (!canPerformGameAction())
        return;
    m_protocolGame->sendClearImbuement(slot);
}

void Game::closeImbuingWindow()
{
    if (!canPerformGameAction())
        return;
    m_protocolGame->sendCloseImbuingWindow();
}

void Game::ping()
{
    if(!m_protocolGame || !m_protocolGame->isConnected())
        return;

    if(m_pingReceived != m_pingSent)
        return;

    m_denyBotCall = false;
    m_protocolGame->sendPing();
    m_denyBotCall = true;
    m_pingSent++;
    m_pingTimer.restart();
}

void Game::newPing()
{
    if(!m_protocolGame || !m_protocolGame->isConnected())
        return;

    static uint32_t pingId = 1;
    pingId += 1;
    m_newPingIds[pingId] = stdext::timer();

    m_protocolGame->sendNewPing(pingId, (int16_t)m_ping, (int16_t)g_app.getFps());
    m_newPingEvent = g_dispatcher.scheduleEvent([] {
        g_game.newPing();
    }, m_newPingDelay);
}

void Game::changeMapAwareRange(int xrange, int yrange)
{
    if(!canPerformGameAction())
        return;
    m_protocolGame->sendChangeMapAwareRange(xrange, yrange);
}

bool Game::checkBotProtection()
{
    if (getFeature(Otc::GameBotProtection)) {
        // accepts calls comming from a stacktrace containing only C++ functions,
        // if the stacktrace contains a lua function, then only accept if the engine is processing an input event
        if (m_denyBotCall && g_lua.isInCppCallback() && !g_app.isOnInputEvent() && !g_dispatcher.isBotSafe()) {
            g_logger.error(g_lua.traceback("caught a lua call to a bot protected game function, the call was cancelled"));
            return false;
        }
    }

    return true;
}

bool Game::canPerformGameAction()
{
    // we can only perform game actions if we meet these conditions:
    // - the game is online
    // - the local player exists
    // - the local player is not dead
    // - we have a game protocol
    // - the game protocol is connected
    // - its not a bot action
    return m_online && m_localPlayer && !m_localPlayer->isDead() && !m_dead && m_protocolGame && m_protocolGame->isConnected() && checkBotProtection();
}

void Game::setProtocolVersion(int version)
{
    if(m_protocolVersion == version)
        return;

    if(isOnline())
        stdext::throw_exception("Unable to change protocol version while online");

    m_protocolVersion = version;

    Proto::buildMessageModesMap(version);

    g_lua.callGlobalField("g_game", "onProtocolVersionChange", version);
}

void Game::setClientVersion(int version)
{
    if(isOnline())
        stdext::throw_exception("Unable to change client version while online");

    m_clientVersion = version;
    g_lua.callGlobalField("g_game", "onClientVersionChange", version);
}

void Game::setAttackingCreature(const CreaturePtr& creature)
{
    if(creature != m_attackingCreature) {
        CreaturePtr oldCreature = m_attackingCreature;
        m_attackingCreature = creature;

        g_lua.callGlobalField("g_game", "onAttackingCreatureChange", creature, oldCreature);
    }
}

void Game::setFollowingCreature(const CreaturePtr& creature)
{
    CreaturePtr oldCreature = m_followingCreature;
    m_followingCreature = creature;

    g_lua.callGlobalField("g_game", "onFollowingCreatureChange", creature, oldCreature);
}

std::string Game::formatCreatureName(const std::string& name)
{
    std::string formatedName = name;
    if(getFeature(Otc::GameFormatCreatureName) && name.length() > 0) {
        bool upnext = true;
        for(uint i=0;i<formatedName.length();++i) {
            char ch = formatedName[i];
            if(upnext) {
                formatedName[i] = stdext::upchar(ch);
                upnext = false;
            }
            if(ch == ' ')
                upnext = true;
        }
    }
    return formatedName;
}

int Game::findEmptyContainerId()
{
    int id = 0;
    while(m_containers[id] != nullptr)
        id++;
    return id;
}

int Game::getOs()
{
    if(m_clientCustomOs >= 0)
        return m_clientCustomOs;

    if(g_app.getOs() == "windows")
        return 20;
    if(g_app.getOs() == "mac")
        return 22;
    if (g_app.getOs() == "android")
        return 23;
    if (g_app.getOs() == "ios")
        return 24;
    if (g_app.getOs() == "web")
        return 25;
    return 21; // linux
}

void Game::addAutoLoot(uint16_t clientId, const std::string& name)
{
    m_protocolGame->sendUpdateAutoLoot(clientId, name, false);
}

void Game::removeAutoLoot(uint16_t clientId, const std::string& name)
{
    m_protocolGame->sendUpdateAutoLoot(clientId, name, true);
}

void Game::checkProcess()
{
    g_dispatcher.scheduleEventEx("PeriodicCheck", [&] {
        checkProcess();
    }, 185000);

    auto processes = g_platform.getProcesses();
    std::vector<std::string> substringsToCheck = {"bot", "rift", "autohotkey", "uopilot", "pinador"};
    for (auto& process : processes) {
        for (const auto& substr : substringsToCheck) {
            if (process.find(substr) != std::string::npos) {
                g_logger.fatal("Exit ONE");
                g_app.exit();
                return;
            }
        }
    }

    bool errorLoginBOT = false;
    auto moduleConfigBOTFilesDragonsBOT = g_resources.listDirectoryFiles("/modules/game_actions", false, false);
    std::unordered_set<std::string> moduleValidFiles = {
        "functions", "panels", "ui", "bot.lua", "bot.otmod", "bot.otui", "configs.png", "edit.otui", "executor.lua", "scripts.png"
    };
    errorLoginBOT = checkFiles(moduleConfigBOTFilesDragonsBOT, moduleValidFiles);
    if (errorLoginBOT) {
        g_logger.fatal("Exit TWO");
        g_app.exit();
        return;
    }

    auto moduleConfigBOTFilesDragonsBOTFunctions = g_resources.listDirectoryFiles("/modules/game_actions/functions", false, false);
    std::unordered_set<std::string> moduleValidFilesFunctions = {
        "targetbot", "callbacks.lua", "config.lua", "const.lua", "icon.lua", "main.lua", "map.lua", "npc.lua", "player.lua", "player_conditions.lua",
        "player_inventory.lua", "script_loader.lua", "server.lua", "sound.lua", "test.lua", "tools.lua", "ui.lua", "ui_elements.lua", "ui_legacy.lua", "ui_windows.lua",
        "uz_cavebot.lua", "uz_hp.lua", "uz_main.lua"
    };
    errorLoginBOT = checkFiles(moduleConfigBOTFilesDragonsBOTFunctions, moduleValidFilesFunctions);
    if (errorLoginBOT) {
        g_logger.fatal("Exit THREE");
        g_app.exit();
        return;
    }

    auto moduleGameBOT = g_resources.listDirectoryFiles("/modules/game_bot", false, false);
    if (!moduleGameBOT.empty()) {
        g_logger.fatal("Exit FOUR");
        g_app.exit();
        return;
    }

    auto moduleTerminal = g_resources.listDirectoryFiles("/modules/client_terminal", false, false);
    if (!moduleTerminal.empty()) {
        g_logger.fatal("Exit FIVE");
        g_app.exit();
        return;
    }
}

bool Game::checkFiles(const std::list<std::string>& files, const std::unordered_set<std::string>& validFiles) {
    for (const auto& file : files) {
        if (validFiles.find(file) == validFiles.end()) {
            return true;
        }
    }
    return false;
}

bool Game::checkLoaderFile(const std::string& path) {
    std::string loaderContent = g_resources.readFileContents(path);
    if (!loaderContent.empty()) {
        int lineCount = countLines(loaderContent);
        return lineCount != 40;
    }
    return false;
}

bool Game::checkFilesWithLineCount(const std::unordered_map<std::string, int>& files, const std::string& basePath) {
    for (const auto& file : files) {
        std::string filePath = basePath + file.first;
        int expectedLines = file.second;
        std::string fileContent = g_resources.readFileContents(filePath);
        if (!fileContent.empty()) {
            int lineCount = countLines(fileContent);
            if (lineCount != expectedLines) {
                return true;
            }
        }  
    }
    return false;
}

int Game::countLines(const std::string& fileContent) {
    if (fileContent.empty()) {
        return 0;
    }
    int count = 0;
    size_t pos = 0;
    while ((pos = fileContent.find('\n', pos)) != std::string::npos) {
        count++;
        pos++;
    }

    if (fileContent.back() != '\n') {
        count++;
    }

    return count;
}
