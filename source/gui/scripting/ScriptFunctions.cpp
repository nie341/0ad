/* Copyright (C) 2017 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "scriptinterface/ScriptInterface.h"

#include "graphics/Camera.h"
#include "graphics/FontMetrics.h"
#include "graphics/GameView.h"
#include "graphics/MapReader.h"
#include "graphics/scripting/JSInterface_GameView.h"
#include "gui/GUI.h"
#include "gui/GUIManager.h"
#include "gui/IGUIObject.h"
#include "gui/scripting/JSInterface_GUITypes.h"
#include "i18n/L10n.h"
#include "i18n/scripting/JSInterface_L10n.h"
#include "lib/external_libraries/enet.h"
#include "lib/svn_revision.h"
#include "lib/sysdep/sysdep.h"
#include "lib/utf8.h"
#include "lobby/scripting/JSInterface_Lobby.h"
#include "lobby/IXmppClient.h"
#include "maths/FixedVector3D.h"
#include "network/NetClient.h"
#include "network/NetMessage.h"
#include "network/NetServer.h"
#include "network/StunClient.h"
#include "ps/CConsole.h"
#include "ps/CLogger.h"
#include "ps/Errors.h"
#include "ps/GUID.h"
#include "ps/Game.h"
#include "ps/GameSetup/Atlas.h"
#include "ps/GameSetup/Config.h"
#include "ps/Globals.h"	// g_frequencyFilter
#include "ps/Hotkey.h"
#include "ps/ProfileViewer.h"
#include "ps/Profile.h"
#include "ps/Pyrogenesis.h"
#include "ps/Replay.h"
#include "ps/SavedGame.h"
#include "ps/UserReport.h"
#include "ps/World.h"
#include "ps/scripting/JSInterface_ConfigDB.h"
#include "ps/scripting/JSInterface_Console.h"
#include "ps/scripting/JSInterface_Mod.h"
#include "ps/scripting/JSInterface_VFS.h"
#include "ps/scripting/JSInterface_VisualReplay.h"
#include "renderer/scripting/JSInterface_Renderer.h"
#include "simulation2/Simulation2.h"
#include "simulation2/components/ICmpAIManager.h"
#include "simulation2/components/ICmpCommandQueue.h"
#include "simulation2/components/ICmpGuiInterface.h"
#include "simulation2/components/ICmpRangeManager.h"
#include "simulation2/components/ICmpSelectable.h"
#include "simulation2/components/ICmpTemplateManager.h"
#include "simulation2/helpers/Selection.h"
#include "simulation2/system/TurnManager.h"
#include "soundmanager/SoundManager.h"
#include "soundmanager/scripting/JSInterface_Sound.h"
#include "tools/atlas/GameInterface/GameLoop.h"

/*
 * This file defines a set of functions that are available to GUI scripts, to allow
 * interaction with the rest of the engine.
 * Functions are exposed to scripts within the global object 'Engine', so
 * scripts should call "Engine.FunctionName(...)" etc.
 */

extern void restart_mainloop_in_atlas(); // from main.cpp
extern void EndGame();
extern void kill_mainloop();

namespace {

// Note that the initData argument may only contain clonable data.
// Functions aren't supported for example!
// TODO: Use LOGERROR to print a friendly error message when the requirements aren't met instead of failing with debug_warn when cloning.
void PushGuiPage(ScriptInterface::CxPrivate* pCxPrivate, const std::wstring& name, JS::HandleValue initData)
{
	g_GUI->PushPage(name, pCxPrivate->pScriptInterface->WriteStructuredClone(initData));
}

void SwitchGuiPage(ScriptInterface::CxPrivate* pCxPrivate, const std::wstring& name, JS::HandleValue initData)
{
	g_GUI->SwitchPage(name, pCxPrivate->pScriptInterface, initData);
}

void PopGuiPage(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	g_GUI->PopPage();
}

// Note that the args argument may only contain clonable data.
// Functions aren't supported for example!
// TODO: Use LOGERROR to print a friendly error message when the requirements aren't met instead of failing with debug_warn when cloning.
void PopGuiPageCB(ScriptInterface::CxPrivate* pCxPrivate, JS::HandleValue args)
{
	g_GUI->PopPageCB(pCxPrivate->pScriptInterface->WriteStructuredClone(args));
}

void ResetCursor(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	g_GUI->ResetCursor();
}

JS::Value GuiInterfaceCall(ScriptInterface::CxPrivate* pCxPrivate, const std::wstring& name, JS::HandleValue data)
{
	if (!g_Game)
		return JS::UndefinedValue();
	CSimulation2* sim = g_Game->GetSimulation2();
	ENSURE(sim);

	CmpPtr<ICmpGuiInterface> cmpGuiInterface(*sim, SYSTEM_ENTITY);
	if (!cmpGuiInterface)
		return JS::UndefinedValue();

	JSContext* cxSim = sim->GetScriptInterface().GetContext();
	JSAutoRequest rqSim(cxSim);
	JS::RootedValue arg(cxSim, sim->GetScriptInterface().CloneValueFromOtherContext(*(pCxPrivate->pScriptInterface), data));
	JS::RootedValue ret(cxSim);
	cmpGuiInterface->ScriptCall(g_Game->GetViewedPlayerID(), name, arg, &ret);

	return pCxPrivate->pScriptInterface->CloneValueFromOtherContext(sim->GetScriptInterface(), ret);
}

void PostNetworkCommand(ScriptInterface::CxPrivate* pCxPrivate, JS::HandleValue cmd)
{
	if (!g_Game)
		return;
	CSimulation2* sim = g_Game->GetSimulation2();
	ENSURE(sim);

	CmpPtr<ICmpCommandQueue> cmpCommandQueue(*sim, SYSTEM_ENTITY);
	if (!cmpCommandQueue)
		return;

	JSContext* cxSim = sim->GetScriptInterface().GetContext();
	JSAutoRequest rqSim(cxSim);
	JS::RootedValue cmd2(cxSim, sim->GetScriptInterface().CloneValueFromOtherContext(*(pCxPrivate->pScriptInterface), cmd));

	cmpCommandQueue->PostNetworkCommand(cmd2);
}

entity_id_t PickEntityAtPoint(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int x, int y)
{
	return EntitySelection::PickEntityAtPoint(*g_Game->GetSimulation2(), *g_Game->GetView()->GetCamera(), x, y, g_Game->GetViewedPlayerID(), false);
}

std::vector<entity_id_t> PickPlayerEntitiesInRect(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int x0, int y0, int x1, int y1, int player)
{
	return EntitySelection::PickEntitiesInRect(*g_Game->GetSimulation2(), *g_Game->GetView()->GetCamera(), x0, y0, x1, y1, player, false);
}

std::vector<entity_id_t> PickPlayerEntitiesOnScreen(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int player)
{
	return EntitySelection::PickEntitiesInRect(*g_Game->GetSimulation2(), *g_Game->GetView()->GetCamera(), 0, 0, g_xres, g_yres, player, false);
}

std::vector<entity_id_t> PickNonGaiaEntitiesOnScreen(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return EntitySelection::PickNonGaiaEntitiesInRect(*g_Game->GetSimulation2(), *g_Game->GetView()->GetCamera(), 0, 0, g_xres, g_yres, false);
}

std::vector<entity_id_t> PickSimilarPlayerEntities(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::string& templateName, bool includeOffScreen, bool matchRank, bool allowFoundations)
{
	return EntitySelection::PickSimilarEntities(*g_Game->GetSimulation2(), *g_Game->GetView()->GetCamera(), templateName, g_Game->GetViewedPlayerID(), includeOffScreen, matchRank, false, allowFoundations);
}

CFixedVector3D GetTerrainAtScreenPoint(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int x, int y)
{
	CVector3D pos = g_Game->GetView()->GetCamera()->GetWorldCoordinates(x, y, true);
	return CFixedVector3D(fixed::FromFloat(pos.X), fixed::FromFloat(pos.Y), fixed::FromFloat(pos.Z));
}

std::wstring SetCursor(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::wstring& name)
{
	std::wstring old = g_CursorName;
	g_CursorName = name;
	return old;
}

bool IsVisualReplay(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	if (!g_Game)
		return false;

	return g_Game->IsVisualReplay();
}

std::wstring GetCurrentReplayDirectory(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	if (!g_Game)
		return std::wstring();

	if (g_Game->IsVisualReplay())
		return g_Game->GetReplayPath().Parent().Filename().string();

	return g_Game->GetReplayLogger().GetDirectory().Filename().string();
}

int GetPlayerID(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	if (g_Game)
		return g_Game->GetPlayerID();
	return -1;
}

void SetPlayerID(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int id)
{
	if (g_Game)
		g_Game->SetPlayerID(id);
}

void SetViewedPlayer(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int id)
{
	if (g_Game)
		g_Game->SetViewedPlayerID(id);
}

JS::Value GetEngineInfo(ScriptInterface::CxPrivate* pCxPrivate)
{
	return SavedGames::GetEngineInfo(*(pCxPrivate->pScriptInterface));
}

JS::Value FindStunEndpoint(ScriptInterface::CxPrivate* pCxPrivate, int port)
{
	return StunClient::FindStunEndpointHost(*(pCxPrivate->pScriptInterface), port);
}

void StartNetworkGame(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	ENSURE(g_NetClient);
	g_NetClient->SendStartGameMessage();
}

void StartGame(ScriptInterface::CxPrivate* pCxPrivate, JS::HandleValue attribs, int playerID)
{
	ENSURE(!g_NetServer);
	ENSURE(!g_NetClient);

	ENSURE(!g_Game);
	g_Game = new CGame();

	// Convert from GUI script context to sim script context
	CSimulation2* sim = g_Game->GetSimulation2();
	JSContext* cxSim = sim->GetScriptInterface().GetContext();
	JSAutoRequest rqSim(cxSim);

	JS::RootedValue gameAttribs(cxSim,
		sim->GetScriptInterface().CloneValueFromOtherContext(*(pCxPrivate->pScriptInterface), attribs));

	g_Game->SetPlayerID(playerID);
	g_Game->StartGame(&gameAttribs, "");
}

JS::Value StartSavedGame(ScriptInterface::CxPrivate* pCxPrivate, const std::wstring& name)
{
	// We need to be careful with different compartments and contexts.
	// The GUI calls this function from the GUI context and expects the return value in the same context.
	// The game we start from here creates another context and expects data in this context.

	JSContext* cxGui = pCxPrivate->pScriptInterface->GetContext();
	JSAutoRequest rq(cxGui);

	ENSURE(!g_NetServer);
	ENSURE(!g_NetClient);

	ENSURE(!g_Game);

	// Load the saved game data from disk
	JS::RootedValue guiContextMetadata(cxGui);
	std::string savedState;
	Status err = SavedGames::Load(name, *(pCxPrivate->pScriptInterface), &guiContextMetadata, savedState);
	if (err < 0)
		return JS::UndefinedValue();

	g_Game = new CGame();

	{
		CSimulation2* sim = g_Game->GetSimulation2();
		JSContext* cxGame = sim->GetScriptInterface().GetContext();
		JSAutoRequest rq(cxGame);

		JS::RootedValue gameContextMetadata(cxGame,
			sim->GetScriptInterface().CloneValueFromOtherContext(*(pCxPrivate->pScriptInterface), guiContextMetadata));
		JS::RootedValue gameInitAttributes(cxGame);
		sim->GetScriptInterface().GetProperty(gameContextMetadata, "initAttributes", &gameInitAttributes);

		int playerID;
		sim->GetScriptInterface().GetProperty(gameContextMetadata, "playerID", playerID);

		// Start the game
		g_Game->SetPlayerID(playerID);
		g_Game->StartGame(&gameInitAttributes, savedState);
	}

	return guiContextMetadata;
}

void SaveGame(ScriptInterface::CxPrivate* pCxPrivate, const std::wstring& filename, const std::wstring& description, JS::HandleValue GUIMetadata)
{
	shared_ptr<ScriptInterface::StructuredClone> GUIMetadataClone = pCxPrivate->pScriptInterface->WriteStructuredClone(GUIMetadata);
	if (SavedGames::Save(filename, description, *g_Game->GetSimulation2(), GUIMetadataClone) < 0)
		LOGERROR("Failed to save game");
}

void SaveGamePrefix(ScriptInterface::CxPrivate* pCxPrivate, const std::wstring& prefix, const std::wstring& description, JS::HandleValue GUIMetadata)
{
	shared_ptr<ScriptInterface::StructuredClone> GUIMetadataClone = pCxPrivate->pScriptInterface->WriteStructuredClone(GUIMetadata);
	if (SavedGames::SavePrefix(prefix, description, *g_Game->GetSimulation2(), GUIMetadataClone) < 0)
		LOGERROR("Failed to save game");
}

void SetNetworkGameAttributes(ScriptInterface::CxPrivate* pCxPrivate, JS::HandleValue attribs1)
{
	ENSURE(g_NetClient);

	//TODO: This is a workaround because we need to pass a MutableHandle to a JSAPI functions somewhere
	// (with no obvious reason).
	JSContext* cx = pCxPrivate->pScriptInterface->GetContext();
	JSAutoRequest rq(cx);
	JS::RootedValue attribs(cx, attribs1);

	g_NetClient->SendGameSetupMessage(&attribs, *(pCxPrivate->pScriptInterface));
}

void StartNetworkHost(ScriptInterface::CxPrivate* pCxPrivate, const CStrW& playerName, const u16 serverPort)
{
	ENSURE(!g_NetClient);
	ENSURE(!g_NetServer);
	ENSURE(!g_Game);

	g_NetServer = new CNetServer();
	if (!g_NetServer->SetupConnection(serverPort))
	{
		pCxPrivate->pScriptInterface->ReportError("Failed to start server");
		SAFE_DELETE(g_NetServer);
		return;
	}

	g_Game = new CGame();
	g_NetClient = new CNetClient(g_Game, true);
	g_NetClient->SetUserName(playerName);

	if (!g_NetClient->SetupConnection("127.0.0.1", serverPort))
	{
		pCxPrivate->pScriptInterface->ReportError("Failed to connect to server");
		SAFE_DELETE(g_NetClient);
		SAFE_DELETE(g_Game);
	}
}

void StartNetworkJoin(ScriptInterface::CxPrivate* pCxPrivate, const CStrW& playerName, const CStr& serverAddress, u16 serverPort, bool useSTUN, const std::string& hostJID)
{
	ENSURE(!g_NetClient);
	ENSURE(!g_NetServer);
	ENSURE(!g_Game);

	ENetHost* enetClient = nullptr;
	if (g_XmppClient && useSTUN)
	{
		// Find an unused port
		for (int i = 0; i < 5 && !enetClient; ++i)
		{
			// Ports below 1024 are privileged on unix
			u16 port = 1024 + rand() % (UINT16_MAX - 1024);
			ENetAddress hostAddr{ENET_HOST_ANY, port};
			enetClient = enet_host_create(&hostAddr, 1, 1, 0, 0);
			++hostAddr.port;
		}

		if (!enetClient)
		{
			pCxPrivate->pScriptInterface->ReportError("Could not find an unused port for the enet STUN client");
			return;
		}

		StunClient::StunEndpoint* stunEndpoint = StunClient::FindStunEndpointJoin(enetClient);
		if (!stunEndpoint)
		{
			pCxPrivate->pScriptInterface->ReportError("Could not find the STUN endpoint");
			return;
		}

		g_XmppClient->SendStunEndpointToHost(stunEndpoint, hostJID);
		delete stunEndpoint;

		SDL_Delay(1000);
	}

	g_Game = new CGame();
	g_NetClient = new CNetClient(g_Game, false);
	g_NetClient->SetUserName(playerName);

	if (g_XmppClient && useSTUN)
		StunClient::SendHolePunchingMessages(enetClient, serverAddress.c_str(), serverPort);

	if (!g_NetClient->SetupConnection(serverAddress, serverPort, enetClient))
	{
		pCxPrivate->pScriptInterface->ReportError("Failed to connect to server");
		SAFE_DELETE(g_NetClient);
		SAFE_DELETE(g_Game);
	}
}

u16 GetDefaultPort(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return PS_DEFAULT_PORT;
}

void DisconnectNetworkGame(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	// TODO: we ought to do async reliable disconnections

	SAFE_DELETE(g_NetServer);
	SAFE_DELETE(g_NetClient);
	SAFE_DELETE(g_Game);
}

std::string GetPlayerGUID(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	if (!g_NetClient)
		return "local";

	return g_NetClient->GetGUID();
}

void KickPlayer(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const CStrW& playerName, bool ban)
{
	ENSURE(g_NetClient);

	g_NetClient->SendKickPlayerMessage(playerName, ban);
}

JS::Value PollNetworkClient(ScriptInterface::CxPrivate* pCxPrivate)
{
	if (!g_NetClient)
		return JS::UndefinedValue();

	// Convert from net client context to GUI script context
	JSContext* cxNet = g_NetClient->GetScriptInterface().GetContext();
	JSAutoRequest rqNet(cxNet);
	JS::RootedValue pollNet(cxNet);
	g_NetClient->GuiPoll(&pollNet);
	return pCxPrivate->pScriptInterface->CloneValueFromOtherContext(g_NetClient->GetScriptInterface(), pollNet);
}

void AssignNetworkPlayer(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int playerID, const std::string& guid)
{
	ENSURE(g_NetClient);

	g_NetClient->SendAssignPlayerMessage(playerID, guid);
}

void ClearAllPlayerReady (ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	ENSURE(g_NetClient);

	g_NetClient->SendClearAllReadyMessage();
}

void SendNetworkChat(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::wstring& message)
{
	ENSURE(g_NetClient);

	g_NetClient->SendChatMessage(message);
}

void SendNetworkReady(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int message)
{
	ENSURE(g_NetClient);

	g_NetClient->SendReadyMessage(message);
}

JS::Value GetAIs(ScriptInterface::CxPrivate* pCxPrivate)
{
	return ICmpAIManager::GetAIs(*(pCxPrivate->pScriptInterface));
}

JS::Value GetSavedGames(ScriptInterface::CxPrivate* pCxPrivate)
{
	return SavedGames::GetSavedGames(*(pCxPrivate->pScriptInterface));
}

bool DeleteSavedGame(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::wstring& name)
{
	return SavedGames::DeleteSavedGame(name);
}

void OpenURL(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::string& url)
{
	sys_open_url(url);
}

std::wstring GetMatchID(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return ps_generate_guid().FromUTF8();
}

void RestartInAtlas(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	restart_mainloop_in_atlas();
}

bool AtlasIsAvailable(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return ATLAS_IsAvailable();
}

bool IsAtlasRunning(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return (g_AtlasGameLoop && g_AtlasGameLoop->running);
}

JS::Value LoadMapSettings(ScriptInterface::CxPrivate* pCxPrivate, const VfsPath& pathname)
{
	JSContext* cx = pCxPrivate->pScriptInterface->GetContext();
	JSAutoRequest rq(cx);

	CMapSummaryReader reader;

	if (reader.LoadMap(pathname) != PSRETURN_OK)
		return JS::UndefinedValue();

	JS::RootedValue settings(cx);
	reader.GetMapSettings(*(pCxPrivate->pScriptInterface), &settings);
	return settings;
}

JS::Value GetInitAttributes(ScriptInterface::CxPrivate* pCxPrivate)
{
	if (!g_Game)
		return JS::UndefinedValue();

	JSContext* cx = g_Game->GetSimulation2()->GetScriptInterface().GetContext();
	JSAutoRequest rq(cx);

	JS::RootedValue initAttribs(cx);
	g_Game->GetSimulation2()->GetInitAttributes(&initAttribs);
	return pCxPrivate->pScriptInterface->CloneValueFromOtherContext(
		g_Game->GetSimulation2()->GetScriptInterface(),
		initAttribs);
}

/**
 * Get the current X coordinate of the camera.
 */
float CameraGetX(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	if (g_Game && g_Game->GetView())
		return g_Game->GetView()->GetCameraX();
	return -1;
}

/**
 * Get the current Z coordinate of the camera.
 */
float CameraGetZ(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	if (g_Game && g_Game->GetView())
		return g_Game->GetView()->GetCameraZ();
	return -1;
}

/**
 * Start / stop camera following mode
 * @param entityid unit id to follow. If zero, stop following mode
 */
void CameraFollow(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), entity_id_t entityid)
{
	if (g_Game && g_Game->GetView())
		g_Game->GetView()->CameraFollow(entityid, false);
}

/**
 * Start / stop first-person camera following mode
 * @param entityid unit id to follow. If zero, stop following mode
 */
void CameraFollowFPS(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), entity_id_t entityid)
{
	if (g_Game && g_Game->GetView())
		g_Game->GetView()->CameraFollow(entityid, true);
}

/**
 * Set the data (position, orientation and zoom) of the camera
 */
void SetCameraData(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), entity_pos_t x, entity_pos_t y, entity_pos_t z, entity_pos_t rotx, entity_pos_t roty, entity_pos_t zoom)
{
	// called from JS; must not fail
	if(!(g_Game && g_Game->GetWorld() && g_Game->GetView() && g_Game->GetWorld()->GetTerrain()))
		return;

	CVector3D Pos = CVector3D(x.ToFloat(), y.ToFloat(), z.ToFloat());
	float RotX = rotx.ToFloat();
	float RotY = roty.ToFloat();
	float Zoom = zoom.ToFloat();

	g_Game->GetView()->SetCamera(Pos, RotX, RotY, Zoom);
}

/// Move camera to a 2D location
void CameraMoveTo(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), entity_pos_t x, entity_pos_t z)
{
	// called from JS; must not fail
	if(!(g_Game && g_Game->GetWorld() && g_Game->GetView() && g_Game->GetWorld()->GetTerrain()))
		return;

	CTerrain* terrain = g_Game->GetWorld()->GetTerrain();

	CVector3D target;
	target.X = x.ToFloat();
	target.Z = z.ToFloat();
	target.Y = terrain->GetExactGroundLevel(target.X, target.Z);

	g_Game->GetView()->MoveCameraTarget(target);
}

entity_id_t GetFollowedEntity(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	if (g_Game && g_Game->GetView())
		return g_Game->GetView()->GetFollowedEntity();

	return INVALID_ENTITY;
}

bool HotkeyIsPressed_(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::string& hotkeyName)
{
	return HotkeyIsPressed(hotkeyName);
}

void DisplayErrorDialog(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::wstring& msg)
{
	debug_DisplayError(msg.c_str(), DE_NO_DEBUG_INFO, NULL, NULL, NULL, 0, NULL, NULL);
}

JS::Value GetProfilerState(ScriptInterface::CxPrivate* pCxPrivate)
{
	return g_ProfileViewer.SaveToJS(*(pCxPrivate->pScriptInterface));
}

bool IsUserReportEnabled(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return g_UserReporter.IsReportingEnabled();
}

void SetUserReportEnabled(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), bool enabled)
{
	g_UserReporter.SetReportingEnabled(enabled);
}

std::string GetUserReportStatus(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return g_UserReporter.GetStatus();
}

void SubmitUserReport(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::string& type, int version, const std::wstring& data)
{
	g_UserReporter.SubmitReport(type.c_str(), version, utf8_from_wstring(data));
}

void SetSimRate(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), float rate)
{
	g_Game->SetSimRate(rate);
}

float GetSimRate(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return g_Game->GetSimRate();
}

void SetTurnLength(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int length)
{
	if (g_NetServer)
		g_NetServer->SetTurnLength(length);
	else
		LOGERROR("Only network host can change turn length");
}

// Focus the game camera on a given position.
void SetCameraTarget(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), float x, float y, float z)
{
	g_Game->GetView()->ResetCameraTarget(CVector3D(x, y, z));
}

// Deliberately cause the game to crash.
// Currently implemented via access violation (read of address 0).
// Useful for testing the crashlog/stack trace code.
int Crash(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	debug_printf("Crashing at user's request.\n");
	return *(volatile int*)0;
}

void DebugWarn(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	debug_warn(L"Warning at user's request.");
}

// Force a JS garbage collection cycle to take place immediately.
// Writes an indication of how long this took to the console.
void ForceGC(ScriptInterface::CxPrivate* pCxPrivate)
{
	double time = timer_Time();
	JS_GC(pCxPrivate->pScriptInterface->GetJSRuntime());
	time = timer_Time() - time;
	g_Console->InsertMessage(fmt::sprintf("Garbage collection completed in: %f", time));
}

void DumpSimState(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	OsPath path = psLogDir()/"sim_dump.txt";
	std::ofstream file (OsString(path).c_str(), std::ofstream::out | std::ofstream::trunc);
	g_Game->GetSimulation2()->DumpDebugState(file);
}

void DumpTerrainMipmap(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	VfsPath filename(L"screenshots/terrainmipmap.png");
	g_Game->GetWorld()->GetTerrain()->GetHeightMipmap().DumpToDisk(filename);
	OsPath realPath;
	g_VFS->GetRealPath(filename, realPath);
	LOGMESSAGERENDER("Terrain mipmap written to '%s'", realPath.string8());
}

void EnableTimeWarpRecording(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), unsigned int numTurns)
{
	g_Game->GetTurnManager()->EnableTimeWarpRecording(numTurns);
}

void RewindTimeWarp(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	g_Game->GetTurnManager()->RewindTimeWarp();
}

void QuickSave(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	g_Game->GetTurnManager()->QuickSave();
}

void QuickLoad(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	g_Game->GetTurnManager()->QuickLoad();
}

void SetBoundingBoxDebugOverlay(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), bool enabled)
{
	ICmpSelectable::ms_EnableDebugOverlays = enabled;
}

void Script_EndGame(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	EndGame();
}

CStrW GetSystemUsername(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return sys_get_user_name();
}

// Cause the game to exit gracefully.
// params:
// returns:
// notes:
// - Exit happens after the current main loop iteration ends
//   (since this only sets a flag telling it to end)
void ExitProgram(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	kill_mainloop();
}

// Is the game paused?
bool IsPaused(ScriptInterface::CxPrivate* pCxPrivate)
{
	if (!g_Game)
	{
		JS_ReportError(pCxPrivate->pScriptInterface->GetContext(), "Game is not started");
		return false;
	}

	return g_Game->m_Paused;
}

// Pause/unpause the game
void SetPaused(ScriptInterface::CxPrivate* pCxPrivate, bool pause, bool sendMessage)
{
	if (!g_Game)
	{
		JS_ReportError(pCxPrivate->pScriptInterface->GetContext(), "Game is not started");
		return;
	}
	g_Game->m_Paused = pause;
#if CONFIG2_AUDIO
	if (g_SoundManager)
		g_SoundManager->Pause(pause);
#endif

	if (g_NetClient && sendMessage)
		g_NetClient->SendPausedMessage(pause);
}

// Return the global frames-per-second value.
// params:
// returns: FPS [int]
// notes:
// - This value is recalculated once a frame. We take special care to
//   filter it, so it is both accurate and free of jitter.
int GetFps(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	int freq = 0;
	if (g_frequencyFilter)
		freq = g_frequencyFilter->StableFrequency();
	return freq;
}

JS::Value GetGUIObjectByName(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const CStr& name)
{
	IGUIObject* guiObj = g_GUI->FindObjectByName(name);
	if (guiObj)
		return JS::ObjectValue(*guiObj->GetJSObject());
	else
		return JS::UndefinedValue();
}

// Return the date/time at which the current executable was compiled.
// params: mode OR an integer specifying
//   what to display: -1 for "date time (svn revision)", 0 for date, 1 for time, 2 for svn revision
// returns: string with the requested timestamp info
// notes:
// - Displayed on main menu screen; tells non-programmers which auto-build
//   they are running. Could also be determined via .EXE file properties,
//   but that's a bit more trouble.
// - To be exact, the date/time returned is when scriptglue.cpp was
//   last compiled, but the auto-build does full rebuilds.
// - svn revision is generated by calling svnversion and cached in
//   lib/svn_revision.cpp. it is useful to know when attempting to
//   reproduce bugs (the main EXE and PDB should be temporarily reverted to
//   that revision so that they match user-submitted crashdumps).
std::wstring GetBuildTimestamp(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), int mode)
{
	char buf[200];
	if (mode == -1) // Date, time and revision.
	{
		UDate dateTime = g_L10n.ParseDateTime(__DATE__ " " __TIME__, "MMM d yyyy HH:mm:ss", Locale::getUS());
		std::string dateTimeString = g_L10n.LocalizeDateTime(dateTime, L10n::DateTime, SimpleDateFormat::DATE_TIME);
		char svnRevision[32];
		sprintf_s(svnRevision, ARRAY_SIZE(svnRevision), "%ls", svn_revision);
		if (strcmp(svnRevision, "custom build") == 0)
		{
			// Translation: First item is a date and time, item between parenthesis is the Subversion revision number of the current build.
			sprintf_s(buf, ARRAY_SIZE(buf), g_L10n.Translate("%s (custom build)").c_str(), dateTimeString.c_str());
		}
		else
		{
			// Translation: First item is a date and time, item between parenthesis is the Subversion revision number of the current build.
			// dennis-ignore: *
			sprintf_s(buf, ARRAY_SIZE(buf), g_L10n.Translate("%s (%ls)").c_str(), dateTimeString.c_str(), svn_revision);
		}
	}
	else if (mode == 0) // Date.
	{
		UDate dateTime = g_L10n.ParseDateTime(__DATE__, "MMM d yyyy", Locale::getUS());
		std::string dateTimeString = g_L10n.LocalizeDateTime(dateTime, L10n::Date, SimpleDateFormat::MEDIUM);
		sprintf_s(buf, ARRAY_SIZE(buf), "%s", dateTimeString.c_str());
	}
	else if (mode == 1) // Time.
	{
		UDate dateTime = g_L10n.ParseDateTime(__TIME__, "HH:mm:ss", Locale::getUS());
		std::string dateTimeString = g_L10n.LocalizeDateTime(dateTime, L10n::Time, SimpleDateFormat::MEDIUM);
		sprintf_s(buf, ARRAY_SIZE(buf), "%s", dateTimeString.c_str());
	}
	else if (mode == 2) // Revision.
	{
		char svnRevision[32];
		sprintf_s(svnRevision, ARRAY_SIZE(svnRevision), "%ls", svn_revision);
		if (strcmp(svnRevision, "custom build") == 0)
		{
			sprintf_s(buf, ARRAY_SIZE(buf), "%s", g_L10n.Translate("custom build").c_str());
		}
		else
		{
			sprintf_s(buf, ARRAY_SIZE(buf), "%ls", svn_revision);
		}
	}

	return wstring_from_utf8(buf);
}

bool TemplateExists(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::string& templateName)
{
	return g_GUI->TemplateExists(templateName);
}

CParamNode GetTemplate(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const std::string& templateName)
{
	return g_GUI->GetTemplate(templateName);
}

int GetTextWidth(ScriptInterface::CxPrivate* UNUSED(pCxPrivate), const CStr& fontName, const CStrW& text)
{
	int width = 0;
	int height = 0;
	CStrIntern _fontName(fontName);
	CFontMetrics fontMetrics(_fontName);
	fontMetrics.CalculateStringSize(text.c_str(), width, height);
	return width;
}

/**
 * Microseconds since the epoch.
 */
double GetMicroseconds(ScriptInterface::CxPrivate* UNUSED(pCxPrivate))
{
	return JS_Now();
}

} // namespace

void GuiScriptingInit(ScriptInterface& scriptInterface)
{
	JSI_IGUIObject::init(scriptInterface);
	JSI_GUITypes::init(scriptInterface);

	JSI_GameView::RegisterScriptFunctions(scriptInterface);
	JSI_Renderer::RegisterScriptFunctions(scriptInterface);
	JSI_Console::RegisterScriptFunctions(scriptInterface);
	JSI_ConfigDB::RegisterScriptFunctions(scriptInterface);
	JSI_Mod::RegisterScriptFunctions(scriptInterface);
	JSI_Sound::RegisterScriptFunctions(scriptInterface);
	JSI_L10n::RegisterScriptFunctions(scriptInterface);
	JSI_Lobby::RegisterScriptFunctions(scriptInterface);
	JSI_VFS::RegisterScriptFunctions(scriptInterface);
	JSI_VisualReplay::RegisterScriptFunctions(scriptInterface);

	// GUI manager functions:
	scriptInterface.RegisterFunction<void, std::wstring, JS::HandleValue, &PushGuiPage>("PushGuiPage");
	scriptInterface.RegisterFunction<void, std::wstring, JS::HandleValue, &SwitchGuiPage>("SwitchGuiPage");
	scriptInterface.RegisterFunction<void, &PopGuiPage>("PopGuiPage");
	scriptInterface.RegisterFunction<void, JS::HandleValue, &PopGuiPageCB>("PopGuiPageCB");
	scriptInterface.RegisterFunction<JS::Value, CStr, &GetGUIObjectByName>("GetGUIObjectByName");
	scriptInterface.RegisterFunction<void, &ResetCursor>("ResetCursor");

	// Simulation<->GUI interface functions:
	scriptInterface.RegisterFunction<JS::Value, std::wstring, JS::HandleValue, &GuiInterfaceCall>("GuiInterfaceCall");
	scriptInterface.RegisterFunction<void, JS::HandleValue, &PostNetworkCommand>("PostNetworkCommand");

	// Entity picking
	scriptInterface.RegisterFunction<entity_id_t, int, int, &PickEntityAtPoint>("PickEntityAtPoint");
	scriptInterface.RegisterFunction<std::vector<entity_id_t>, int, int, int, int, int, &PickPlayerEntitiesInRect>("PickPlayerEntitiesInRect");
	scriptInterface.RegisterFunction<std::vector<entity_id_t>, int, &PickPlayerEntitiesOnScreen>("PickPlayerEntitiesOnScreen");
	scriptInterface.RegisterFunction<std::vector<entity_id_t>, &PickNonGaiaEntitiesOnScreen>("PickNonGaiaEntitiesOnScreen");
	scriptInterface.RegisterFunction<std::vector<entity_id_t>, std::string, bool, bool, bool, &PickSimilarPlayerEntities>("PickSimilarPlayerEntities");
	scriptInterface.RegisterFunction<CFixedVector3D, int, int, &GetTerrainAtScreenPoint>("GetTerrainAtScreenPoint");

	// Network / game setup functions
	scriptInterface.RegisterFunction<void, &StartNetworkGame>("StartNetworkGame");
	scriptInterface.RegisterFunction<void, JS::HandleValue, int, &StartGame>("StartGame");
	scriptInterface.RegisterFunction<void, &Script_EndGame>("EndGame");
	scriptInterface.RegisterFunction<void, CStrW, u16, &StartNetworkHost>("StartNetworkHost");
	scriptInterface.RegisterFunction<void, CStrW, CStr, u16, bool, std::string, &StartNetworkJoin>("StartNetworkJoin");
	scriptInterface.RegisterFunction<u16, &GetDefaultPort>("GetDefaultPort");
	scriptInterface.RegisterFunction<void, &DisconnectNetworkGame>("DisconnectNetworkGame");
	scriptInterface.RegisterFunction<std::string, &GetPlayerGUID>("GetPlayerGUID");
	scriptInterface.RegisterFunction<void, CStrW, bool, &KickPlayer>("KickPlayer");
	scriptInterface.RegisterFunction<JS::Value, &PollNetworkClient>("PollNetworkClient");
	scriptInterface.RegisterFunction<void, JS::HandleValue, &SetNetworkGameAttributes>("SetNetworkGameAttributes");
	scriptInterface.RegisterFunction<void, int, std::string, &AssignNetworkPlayer>("AssignNetworkPlayer");
	scriptInterface.RegisterFunction<void, &ClearAllPlayerReady>("ClearAllPlayerReady");
	scriptInterface.RegisterFunction<void, std::wstring, &SendNetworkChat>("SendNetworkChat");
	scriptInterface.RegisterFunction<void, int, &SendNetworkReady>("SendNetworkReady");
	scriptInterface.RegisterFunction<JS::Value, &GetAIs>("GetAIs");
	scriptInterface.RegisterFunction<JS::Value, &GetEngineInfo>("GetEngineInfo");
	scriptInterface.RegisterFunction<JS::Value, int, &FindStunEndpoint>("FindStunEndpoint");

	// Saved games
	scriptInterface.RegisterFunction<JS::Value, std::wstring, &StartSavedGame>("StartSavedGame");
	scriptInterface.RegisterFunction<JS::Value, &GetSavedGames>("GetSavedGames");
	scriptInterface.RegisterFunction<bool, std::wstring, &DeleteSavedGame>("DeleteSavedGame");
	scriptInterface.RegisterFunction<void, std::wstring, std::wstring, JS::HandleValue, &SaveGame>("SaveGame");
	scriptInterface.RegisterFunction<void, std::wstring, std::wstring, JS::HandleValue, &SaveGamePrefix>("SaveGamePrefix");
	scriptInterface.RegisterFunction<void, &QuickSave>("QuickSave");
	scriptInterface.RegisterFunction<void, &QuickLoad>("QuickLoad");

	// Misc functions
	scriptInterface.RegisterFunction<std::wstring, std::wstring, &SetCursor>("SetCursor");
	scriptInterface.RegisterFunction<bool, &IsVisualReplay>("IsVisualReplay");
	scriptInterface.RegisterFunction<std::wstring, &GetCurrentReplayDirectory>("GetCurrentReplayDirectory");
	scriptInterface.RegisterFunction<int, &GetPlayerID>("GetPlayerID");
	scriptInterface.RegisterFunction<void, int, &SetPlayerID>("SetPlayerID");
	scriptInterface.RegisterFunction<void, int, &SetViewedPlayer>("SetViewedPlayer");
	scriptInterface.RegisterFunction<void, std::string, &OpenURL>("OpenURL");
	scriptInterface.RegisterFunction<std::wstring, &GetMatchID>("GetMatchID");
	scriptInterface.RegisterFunction<void, &RestartInAtlas>("RestartInAtlas");
	scriptInterface.RegisterFunction<bool, &AtlasIsAvailable>("AtlasIsAvailable");
	scriptInterface.RegisterFunction<bool, &IsAtlasRunning>("IsAtlasRunning");
	scriptInterface.RegisterFunction<JS::Value, VfsPath, &LoadMapSettings>("LoadMapSettings");
	scriptInterface.RegisterFunction<JS::Value, &GetInitAttributes>("GetInitAttributes");
	scriptInterface.RegisterFunction<float, &CameraGetX>("CameraGetX");
	scriptInterface.RegisterFunction<float, &CameraGetZ>("CameraGetZ");
	scriptInterface.RegisterFunction<void, entity_id_t, &CameraFollow>("CameraFollow");
	scriptInterface.RegisterFunction<void, entity_id_t, &CameraFollowFPS>("CameraFollowFPS");
	scriptInterface.RegisterFunction<void, entity_pos_t, entity_pos_t, entity_pos_t, entity_pos_t, entity_pos_t, entity_pos_t, &SetCameraData>("SetCameraData");
	scriptInterface.RegisterFunction<void, entity_pos_t, entity_pos_t, &CameraMoveTo>("CameraMoveTo");
	scriptInterface.RegisterFunction<entity_id_t, &GetFollowedEntity>("GetFollowedEntity");
	scriptInterface.RegisterFunction<bool, std::string, &HotkeyIsPressed_>("HotkeyIsPressed");
	scriptInterface.RegisterFunction<void, std::wstring, &DisplayErrorDialog>("DisplayErrorDialog");
	scriptInterface.RegisterFunction<JS::Value, &GetProfilerState>("GetProfilerState");
	scriptInterface.RegisterFunction<void, &ExitProgram>("Exit");
	scriptInterface.RegisterFunction<bool, &IsPaused>("IsPaused");
	scriptInterface.RegisterFunction<void, bool, bool, &SetPaused>("SetPaused");
	scriptInterface.RegisterFunction<int, &GetFps>("GetFPS");
	scriptInterface.RegisterFunction<std::wstring, int, &GetBuildTimestamp>("GetBuildTimestamp");
	scriptInterface.RegisterFunction<bool, std::string, &TemplateExists>("TemplateExists");
	scriptInterface.RegisterFunction<CParamNode, std::string, &GetTemplate>("GetTemplate");
	scriptInterface.RegisterFunction<int, CStr, CStrW, &GetTextWidth>("GetTextWidth");

	// User report functions
	scriptInterface.RegisterFunction<bool, &IsUserReportEnabled>("IsUserReportEnabled");
	scriptInterface.RegisterFunction<void, bool, &SetUserReportEnabled>("SetUserReportEnabled");
	scriptInterface.RegisterFunction<std::string, &GetUserReportStatus>("GetUserReportStatus");
	scriptInterface.RegisterFunction<void, std::string, int, std::wstring, &SubmitUserReport>("SubmitUserReport");

	// Development/debugging functions
	scriptInterface.RegisterFunction<double, &GetMicroseconds>("GetMicroseconds");
	scriptInterface.RegisterFunction<void, float, &SetSimRate>("SetSimRate");
	scriptInterface.RegisterFunction<float, &GetSimRate>("GetSimRate");
	scriptInterface.RegisterFunction<void, int, &SetTurnLength>("SetTurnLength");
	scriptInterface.RegisterFunction<void, float, float, float, &SetCameraTarget>("SetCameraTarget");
	scriptInterface.RegisterFunction<int, &Crash>("Crash");
	scriptInterface.RegisterFunction<void, &DebugWarn>("DebugWarn");
	scriptInterface.RegisterFunction<void, &ForceGC>("ForceGC");
	scriptInterface.RegisterFunction<void, &DumpSimState>("DumpSimState");
	scriptInterface.RegisterFunction<void, &DumpTerrainMipmap>("DumpTerrainMipmap");
	scriptInterface.RegisterFunction<void, unsigned int, &EnableTimeWarpRecording>("EnableTimeWarpRecording");
	scriptInterface.RegisterFunction<void, &RewindTimeWarp>("RewindTimeWarp");
	scriptInterface.RegisterFunction<void, bool, &SetBoundingBoxDebugOverlay>("SetBoundingBoxDebugOverlay");
	scriptInterface.RegisterFunction<CStrW, &GetSystemUsername>("GetSystemUsername");
}
