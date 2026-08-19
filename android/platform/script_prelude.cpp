/*
 *  script_prelude.cpp -- Lua stand-ins for the script API this source
 *  snapshot does not have.
 *
 *  The retail data's scripts (game.db Scripts table, scripts/*.l) were written
 *  against the shipping engine's script API, which grew ~100 functions after
 *  this January-2003 snapshot (see tools: the list below was produced by
 *  diffing every `Name(` in the retail scripts against ScriptFunctions.cpp).
 *  In Lua 4 a call to a nil global aborts the whole script, so one missing
 *  function silences a mission's script entirely.  This prelude, run after the
 *  auto-load scripts, defines the missing names: a few in Lua on top of the API
 *  we have, the rest as no-ops that report themselves once through out(),
 *  so what a mission actually needs shows up in the console.
 *
 *  Lua 4.0 syntax: `for i,v in t do`, getn(), random(), no `#`, varargs via arg.
 */
#include <string>
#include <string.h>

namespace
{
/*  The engine's Lua has no standard library at all (no getglobal/setglobal/
 *  getn -- only what ScriptFunctions.cpp registers: out, Sleep, StartThread,
 *  random, ...), so every stand-in is spelled out. */
struct SStub { const char *pszName; const char *pszReturn; };
const SStub STUBS[] = {
    { "PlaySound", "nil" }, { "Play3DSound", "nil" }, { "StopSound", "nil" }, { "PlayVideo", "nil" },
    { "PlayEffect", "nil" }, { "SetAmbientEffect", "nil" }, { "AttachEffectToUnitBone", "nil" }, { "AttachEffectToWaypoint", "nil" },
    { "FadeIn", "nil" }, { "FadeOut", "nil" }, { "CameraLock", "nil" }, { "CameraSequence", "nil" }, { "CameraSetClipping", "nil" },
    { "AddHints", "nil" }, { "ShowHint", "nil" }, { "ShowLeaveZoneDialog", "nil" }, { "ShowLoseDialog", "nil" },
    { "BeginZone", "nil" }, { "LeaveToSubZone", "nil" }, { "SetLeaveZoneMode", "nil" }, { "SetFirstMissionMode", "nil" }, { "SetTutorialMode", "nil" },
    { "StartGameEx", "nil" }, { "StartGameWithSequence", "nil" }, { "DelayGameStartEx", "nil" }, { "DividedDeployDemo", "nil" },
    { "SetTimeOfDay", "nil" }, { "SetMaxCriticalSeverity", "nil" }, { "SlowSyncAIMap", "nil" }, { "WantTurnBased", "nil" }, { "PlayerGiveTurn", "nil" },
    { "ScenarioAddGoal", "nil" }, { "ScenarioClueGive", "nil" }, { "ScenarioSetGoalComplete", "nil" }, { "ScenarioSetTaskComplete", "nil" },
    { "ButtonCreateState", "nil" }, { "CreateWindow", "nil" }, { "GetWindow", "nil" }, { "IsUIActionIDPresent", "0" },
    { "CreateAndActivateItem", "nil" }, { "DestroyItemInHand", "nil" }, { "FindItem", "nil" }, { "ItemSetToWaypoint", "nil" }, { "ItemUnload", "nil" },
    { "ObjectGetDestroyStage", "0" }, { "ObjectGetHP", "100" }, { "ObjectLockDoor", "nil" }, { "ObjectUnlockDoor", "nil" }, { "ObjectRestoreFromPocket", "nil" },
    { "PassCalcerIsActive", "0" }, { "GetScenarioNumber", "0" }, { "GetUnitPK", "nil" }, { "PlayerGetUnitsEx", "nil" },
    { "HasInventoryItemGroup", "0" }, { "HasInventoryItemUnit", "0" },
    { "UnitApplyTableCritical", "nil" }, { "UnitAttackWaypoint", "nil" }, { "UnitCreateItem", "nil" }, { "UnitDrawWeapon", "nil" }, { "UnitFlyToWaypoint", "nil" },
    { "UnitGetSkill", "0" }, { "UnitGetSkillMaxValue", "0" }, { "UnitGetToHitUnit", "0" }, { "UnitGetToHitWaypoint", "0" },
    { "UnitGiveRandomPerks", "nil" }, { "UnitGrenadeToUnit", "nil" }, { "UnitHealCriticals", "nil" }, { "UnitHoldItem", "nil" }, { "UnitInArea", "0" },
    { "UnitIsCarryingCorpse", "0" }, { "UnitIsHearUnit", "0" }, { "UnitIsUsingCannon", "0" }, { "UnitIsWeaponInHand", "0" }, { "UnitIsWearingPK", "0" },
    { "UnitKeepMoving", "nil" }, { "UnitLeavePK", "nil" }, { "UnitLockPose", "nil" }, { "UnitRegenerateVP", "nil" },
    { "UnitSetCivilianLogic", "nil" }, { "UnitSetFearLogic", "nil" }, { "UnitSetGuardLogic", "nil" }, { "UnitSetHideProbability", "nil" }, { "UnitSetNormalLogic", "nil" },
    { "UnitSetPanicLogic", "nil" }, { "UnitSetRetreatLogic", "nil" }, { "UnitSetScriptLogic", "nil" }, { "UnitSetSkill", "nil" }, { "UnitSetSkillMaxValue", "nil" }, { "UnitSetToHit", "nil" },
    { "UnitShootPrepare", "nil" }, { "UnitStop", "nil" }, { "UnitSwitchToGrenade", "nil" }, { "UnitTakeItem", "nil" }, { "UnitTakeObject", "nil" }, { "UnitWearPK", "nil" },
};
std::string g_szPrelude;
}  // namespace

extern "C" const char *a5_script_prelude( void )
{
    if ( !g_szPrelude.empty() )
        return g_szPrelude.c_str();
    std::string &s = g_szPrelude;
    s += "-- [android] script prelude: stand-ins for API this snapshot lacks (Lua 4, no standard library)\n";
    s += "a5_stub_seen = {}\n";
    s += "function a5_stub_report( name )\n"
         "  if not a5_stub_seen[ name ] then\n"
         "    a5_stub_seen[ name ] = 1\n"
         "    out( '[android] script stub called: ' .. name .. '\\n' )\n"
         "  end\n"
         "end\n";
    /* implementable on the API we have */
    s += "if not WaitForObject then function WaitForObject( o ) if o then while ObjectIsAction( o ) do Sleep( 50 ) end end end end\n";
    s += "if not Random then function Random( a, b ) if b then return random( a, b ) elseif a then return random( a ) else return random() end end end\n";
    s += "if not TableGetSize then function TableGetSize( t ) local n = 0 if t then for i, v in t do n = n + 1 end end return n end end\n";
    s += "a5_game_vars = {}\n";
    s += "if not GetGlobalGameVar then function GetGlobalGameVar( n ) return a5_game_vars[ n ] end end\n";
    s += "if not SetGlobalGameVar then function SetGlobalGameVar( n, v ) a5_game_vars[ n ] = v end end\n";
    s += "if not WaitForInterface then function WaitForInterface() Sleep( 50 ) end end\n";
    s += "if not Pause then function Pause() end end\n";
    /* no-ops, reported once */
    for ( size_t i = 0; i < sizeof( STUBS ) / sizeof( STUBS[ 0 ] ); ++i )
    {
        s += "if not "; s += STUBS[ i ].pszName; s += " then function "; s += STUBS[ i ].pszName;
        s += "( ... ) a5_stub_report( '"; s += STUBS[ i ].pszName; s += "' ) return "; s += STUBS[ i ].pszReturn; s += " end end\n";
    }
    s += "out( '[android] script prelude loaded\\n' )\n";
    return s.c_str();
}
