// Dusklight multiplayer UI + in-world nameplates (Path B).
//
// Two pieces:
//   ShowMultiplayerWindow()     - control panel: host/port/name, connect, list.
//   ShowMultiplayerNameplates() - always-on world-anchored nameplate + marker
//                                 for each remote player in the same scene,
//                                 projected through the game camera.

#include "imgui.h"

#include "ImGuiMenuTools.hpp"
#include "dusk/net.h"
#include "dusk/main.h"                       // dusk::IsGameLaunched

#include "d/d_com_inf_game.h"                // dComIfGd_getView()
#include "d/actor/d_a_player.h"              // daPy_getLinkPlayerActorClass()
#include "d/actor/d_a_dusk_puppet.h"         // daDuskPuppet_remotesVisible()
#include "m_Do/m_Do_lib.h"                   // mDoLib_project / mDoLib_pos2camera
#include "m_Do/m_Do_graphic.h"               // mDoGph_gInf_c
#include "SSystem/SComponent/c_xyz.h"        // cXyz / Vec

#include <cmath>
#include <cstdio>
#include <cstring>

namespace dusk {
namespace {

// Persistent UI buffers for the control window.
char  s_host[64]  = "127.0.0.1";
int   s_port      = 10020;
char  s_name[16]  = "Player";

// Nameplate tuning.
constexpr float kHeadOffset = 200.0f;  // world units above the player's feet

// Project a world point to ImGui display coordinates. Returns false if the
// point is behind the camera or there is no active view.
bool worldToScreen(const cXyz& world, ImVec2& out) {
    if (dComIfGd_getView() == nullptr) {
        return false;
    }

    // Behind-camera rejection: camera-space z is negative in front (matches
    // d_a_alink_kandelaar / d_a_obj_ari, which clamp z to <= 0).
    cXyz camSpace;
    mDoLib_pos2camera(const_cast<Vec*>(static_cast<const Vec*>(&world)), &camSpace);
    if (camSpace.z >= -1.0f) {
        return false;
    }

    cXyz proj;
    mDoLib_project(const_cast<Vec*>(static_cast<const Vec*>(&world)), &proj);

    // mDoLib_project yields framebuffer-pixel coords spanning
    // [getMinXF, getMinXF + getWidthF] x [getMinYF, getMinYF + getHeightF].
    const float w = mDoGph_gInf_c::getWidthF();
    const float h = mDoGph_gInf_c::getHeightF();
    if (w <= 0.0f || h <= 0.0f) {
        return false;
    }
    const float nx = (proj.x - mDoGph_gInf_c::getMinXF()) / w;
    const float ny = (proj.y - mDoGph_gInf_c::getMinYF()) / h;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    out.x = nx * disp.x;
    out.y = ny * disp.y;
    return true;
}

ImU32 colorForId(uint8_t id) {
    // Deterministic, well-spread hue per player id.
    const float hue = std::fmod(0.61803398875f * float(id), 1.0f);
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.65f, 1.0f, r, g, b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1.0f));
}

}  // namespace

void ImGuiMenuTools::ShowMultiplayerNameplates() {
    if (!dusk::IsGameLaunched || !daDuskPuppet_remotesVisible()) {
        return;  // same gate as the puppets: nothing during title demo / cutscenes
    }

    const uint32_t localScene = dusk::net::getLocalSceneHash();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const int count = dusk::net::getRemotePlayerCount();

    for (int i = 0; i < count; ++i) {
        const dusk::net::PlayerState* p = dusk::net::getRemotePlayer(i);
        if (p == nullptr || p->sceneHash != localScene) {
            continue;  // only draw players in the same area
        }

        const cXyz feet(p->posX, p->posY, p->posZ);
        const cXyz head(p->posX, p->posY + kHeadOffset, p->posZ);

        ImVec2 headScr;
        if (!worldToScreen(head, headScr)) {
            continue;
        }

        const ImU32 col = colorForId(p->id);

        // Marker stalk from feet to head, if the feet are also on-screen.
        ImVec2 feetScr;
        if (worldToScreen(feet, feetScr)) {
            dl->AddLine(feetScr, headScr, (col & 0x00FFFFFF) | 0x80000000, 2.0f);
            dl->AddCircleFilled(feetScr, 4.0f, col, 12);
        }

        // Nameplate centered above the head.
        const char* label = (p->name[0] != '\0') ? p->name : "player";
        const ImVec2 ts = ImGui::CalcTextSize(label);
        const ImVec2 pad(4.0f, 2.0f);
        const ImVec2 tl(headScr.x - ts.x * 0.5f - pad.x, headScr.y - ts.y - pad.y * 2.0f);
        const ImVec2 br(headScr.x + ts.x * 0.5f + pad.x, headScr.y);
        dl->AddRectFilled(tl, br, IM_COL32(0, 0, 0, 160), 3.0f);
        dl->AddRect(tl, br, col, 3.0f, 0, 1.5f);
        dl->AddText(ImVec2(headScr.x - ts.x * 0.5f, headScr.y - ts.y - pad.y), col, label);
    }
}

void ImGuiMenuTools::ShowMultiplayerWindow() {
    if (!m_showMultiplayer) {
        return;
    }
    if (!ImGui::Begin("Multiplayer", &m_showMultiplayer)) {
        ImGui::End();
        return;
    }

    const bool enabled   = dusk::net::isEnabled();
    const bool connected = dusk::net::isConnected();

    ImGui::SeparatorText("Connection");
    ImGui::BeginDisabled(enabled);
    ImGui::InputText("Host", s_host, sizeof(s_host));
    ImGui::InputInt("Port", &s_port);
    if (s_port < 1)     s_port = 1;
    if (s_port > 65535) s_port = 65535;
    ImGui::InputText("Name", s_name, sizeof(s_name));
    ImGui::EndDisabled();

    if (!enabled) {
        if (ImGui::Button("Connect", ImVec2(-1, 0))) {
            dusk::net::connect(s_host, s_port, s_name);
        }
    } else {
        if (ImGui::Button("Disconnect", ImVec2(-1, 0))) {
            dusk::net::disconnect();
        }
    }

    ImGui::SeparatorText("Status");
    if (!enabled) {
        ImGui::TextDisabled("Offline");
    } else if (!connected) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Connecting to %s:%d ...", s_host, s_port);
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "Connected as %s (id %u)",
                           dusk::net::getLocalName(), dusk::net::getLocalId());
    }

    if (connected) {
        const int count = dusk::net::getRemotePlayerCount();
        ImGui::SeparatorText("Players");
        ImGui::Text("Remote players: %d", count);

        cXyz myPos(0, 0, 0);
        daPy_py_c* link = daPy_getLinkPlayerActorClass();
        if (link != nullptr) {
            myPos = link->current.pos;
        }
        const uint32_t localScene = dusk::net::getLocalSceneHash();

        if (ImGui::BeginTable("mp_players", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("ID");
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Area");
            ImGui::TableSetupColumn("Dist");
            ImGui::TableHeadersRow();
            for (int i = 0; i < count; ++i) {
                const dusk::net::PlayerState* p = dusk::net::getRemotePlayer(i);
                if (p == nullptr) {
                    continue;
                }
                const bool sameArea = (p->sceneHash == localScene);
                float dist = 0.0f;
                if (sameArea && link != nullptr) {
                    const float dx = p->posX - myPos.x;
                    const float dy = p->posY - myPos.y;
                    const float dz = p->posZ - myPos.z;
                    dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%u", p->id);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(p->name);
                ImGui::TableNextColumn();
                ImGui::TextColored(sameArea ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
                                   sameArea ? "here" : "elsewhere");
                ImGui::TableNextColumn();
                if (sameArea) ImGui::Text("%.0f", dist); else ImGui::TextDisabled("-");
            }
            ImGui::EndTable();
        }
    }

    ImGui::End();
}

}  // namespace dusk
