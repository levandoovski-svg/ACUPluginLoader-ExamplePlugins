#include "pch.h"
#include "ExactPositionTeleportPlugin.h"

#include "ACU/ACUGetSingletons.h"
#include "ACU/Entity.h"
#include "ACU/Human.h"
#include "ACU_DefineNativeFunction.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <shlobj.h>

namespace
{
    constexpr int kTeleportKey = 'T'; // VK_T

    // ACUFixes-0.9.3 addresses for the build represented by the runtime dump
    // in MODLOG/breakfall-gate.md (base 0x140000000, no ASLR).
    //
    // BaseEntity::SetTransform(Entity* this, const Matrix4f* newTransform)
    // copies the supplied matrix into the entity's main transform. The
    // ACUFixes hook confirms the RCX/RDX calling convention.
    DEFINE_GAME_FUNCTION(BaseEntity__SetTransform, 0x1425DE360, void, __fastcall,
        (Entity* entity, const Matrix4f* newTransform));

    // This is the reset call used by ACUFixes-0.9.3's debug TeleportPlayer
    // helper after it writes the player's position.
    DEFINE_GAME_FUNCTION(Human__Dispatch_HSTE_ResetBehavior, 0x14131A530, char, __fastcall,
        (Human* human));

    bool BytesMatch(uintptr_t address, const unsigned char* expected, size_t count)
    {
        __try
        {
            return std::memcmp(reinterpret_cast<const void*>(address), expected, count) == 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    Entity* GetPlayerSafely()
    {
        __try
        {
            return ACU::GetPlayer();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    Human* GetPlayerHumanSafely()
    {
        __try
        {
            return Human::GetForPlayer();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return nullptr;
        }
    }

    bool ApplyNativeSetTransform(Entity* entity, const Matrix4f* transform)
    {
        __try
        {
            BaseEntity__SetTransform(entity, transform);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool DispatchHumanReset(Human* human)
    {
        __try
        {
            Human__Dispatch_HSTE_ResetBehavior(human);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }
}

void ExactPositionTeleportPlugin::Init()
{
    // Fail closed if this DLL is loaded into another ACU build. These are
    // exact signatures observed in the user's runtime dump, not guessed.
    static const unsigned char expectedSetTransform[] = {
        0x0F, 0x28, 0x02, 0x0F, 0x29, 0x41, 0x20, 0x0F
    };
    static const unsigned char expectedResetBehavior[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20
    };

    const bool setTransformMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(BaseEntity__SetTransform),
        expectedSetTransform,
        sizeof(expectedSetTransform));
    const bool resetBehaviorMatches = BytesMatch(
        reinterpret_cast<uintptr_t>(Human__Dispatch_HSTE_ResetBehavior),
        expectedResetBehavior,
        sizeof(expectedResetBehavior));

    m_BuildCompatible = setTransformMatches && resetBehaviorMatches;
    if (m_BuildCompatible)
    {
        SetStatus("Ready - press T to teleport to the exact current position");
    }
    else
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "ABORT: build signature mismatch (SetTransform=%s, ResetBehavior=%s)",
            setTransformMatches ? "ok" : "bad",
            resetBehaviorMatches ? "ok" : "bad");
        SetStatus(details);
    }
}

void ExactPositionTeleportPlugin::OnUpdate()
{
    const bool keyDown = (GetAsyncKeyState(kTeleportKey) & 0x8000) != 0;
    if (keyDown && !m_PrevKeyDown)
    {
        TeleportToExactCurrentPosition();
    }
    m_PrevKeyDown = keyDown;
}

void ExactPositionTeleportPlugin::TeleportToExactCurrentPosition()
{
    m_LastTransformApplied = false;
    m_LastResetDispatched = false;

    if (!m_BuildCompatible)
    {
        SetStatus("Teleport skipped: incompatible ACU build");
        return;
    }

    Entity* player = GetPlayerSafely();
    if (!player)
    {
        SetStatus("Teleport skipped: player entity is unavailable");
        return;
    }

    // Snapshot before writing. The destination is exactly the position that
    // was sampled; no offset or movement is introduced.
    const Vector3f exactPosition = player->GetPosition();
    const Matrix4f exactTransform = player->GetTransform();

    m_LastX = exactPosition.x;
    m_LastY = exactPosition.y;
    m_LastZ = exactPosition.z;

    // Keep the library-proven position write, then also send the unchanged
    // matrix through the native transform setter so this is a real transform
    // update rather than merely a same-value assignment that the compiler
    // could discard.
    player->GetPosition() = exactPosition;
    m_LastTransformApplied = ApplyNativeSetTransform(player, &exactTransform);

    if (!m_LastTransformApplied)
    {
        SetStatus("Teleport failed: native SetTransform raised an exception");
        return;
    }

    Human* human = GetPlayerHumanSafely();
    if (!human)
    {
        char details[192];
        std::snprintf(details, sizeof(details),
            "Transform applied at (%.3f, %.3f, %.3f), but player Human was unavailable",
            m_LastX, m_LastY, m_LastZ);
        SetStatus(details);
        return;
    }

    // This is the same HSTE reset used by ACUFixes' debug TeleportPlayer.
    // It lets the game cancel/re-evaluate the current parkour behavior after
    // seeing the transformed position.
    m_LastResetDispatched = DispatchHumanReset(human);

    char details[224];
    std::snprintf(details, sizeof(details),
        "Teleported to exact position (%.3f, %.3f, %.3f); reset %s",
        m_LastX, m_LastY, m_LastZ,
        m_LastResetDispatched ? "dispatched" : "failed");
    SetStatus(details);
}

void ExactPositionTeleportPlugin::OnImGuiRender()
{
    ImGui::Begin("Exact Position Teleport");
    ImGui::Text("Press T to teleport Arno to his exact current coordinates.");
    ImGui::Text("Build signature: %s", m_BuildCompatible ? "compatible" : "MISMATCH");
    ImGui::Text("Last transform applied: %s", m_LastTransformApplied ? "yes" : "no");
    ImGui::Text("Last HSTE reset dispatched: %s", m_LastResetDispatched ? "yes" : "no");
    ImGui::Text("Last position: %.3f, %.3f, %.3f", m_LastX, m_LastY, m_LastZ);
    ImGui::Separator();
    ImGui::TextWrapped("Last result: %s", m_LastStatus.c_str());
    if (ImGui::Button("Teleport now"))
    {
        TeleportToExactCurrentPosition();
    }
    ImGui::End();
}

void ExactPositionTeleportPlugin::SetStatus(const std::string& status)
{
    m_LastStatus = status;
    Log(status);
}

void ExactPositionTeleportPlugin::Log(const std::string& line) const
{
    const std::string text = "[ExactPositionTeleport] " + line + "\n";
    OutputDebugStringA(text.c_str());

    try
    {
        std::ofstream file(GetLogPath(), std::ios::app);
        if (file)
        {
            file << text;
        }
    }
    catch (...)
    {
    }
}

std::string ExactPositionTeleportPlugin::GetLogPath() const
{
    char documents[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, documents)))
    {
        return std::string(documents) + "\\Assassin's Creed Unity\\ExactPositionTeleport.log";
    }
    return "ExactPositionTeleport.log";
}
