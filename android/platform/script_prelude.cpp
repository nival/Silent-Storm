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
extern "C" const char *a5_script_prelude( void )
{
    return
        "-- [android] script prelude: stand-ins for API this snapshot lacks\n"
        "a5_stub_seen = {}\n"
        "function a5_stub( name, ret )\n"
        "  if getglobal( name ) then return end\n"
        "  setglobal( name, function( ... )\n"
        "    if not a5_stub_seen[ %name ] then\n"
        "      a5_stub_seen[ %name ] = 1\n"
        "      out( '[android] script stub called: ' .. %name .. '\\n' )\n"
        "    end\n"
        "    return %ret\n"
        "  end )\n"
        "end\n"
        /* implementable in Lua */
        "if not WaitForObject then function WaitForObject( o ) if o then while ObjectIsAction( o ) do Sleep( 50 ) end end end end\n"
        "if not Random then function Random( a, b ) if b then return random( a, b ) elseif a then return random( a ) else return random() end end end\n"
        "if not TableGetSize then function TableGetSize( t ) if t then return getn( t ) else return 0 end end end\n"
        "a5_game_vars = {}\n"
        "if not GetGlobalGameVar then function GetGlobalGameVar( n ) return a5_game_vars[ n ] end end\n"
        "if not SetGlobalGameVar then function SetGlobalGameVar( n, v ) a5_game_vars[ n ] = v end end\n"
        "if not WaitForInterface then function WaitForInterface() Sleep( 50 ) end end\n"
        "if not Pause then function Pause() end end\n"
        /* no-ops, reported once */
        "a5_stub( 'PlaySound' ) a5_stub( 'Play3DSound' ) a5_stub( 'StopSound' ) a5_stub( 'PlayVideo' )\n"
        "a5_stub( 'PlayEffect' ) a5_stub( 'SetAmbientEffect' ) a5_stub( 'AttachEffectToUnitBone' ) a5_stub( 'AttachEffectToWaypoint' )\n"
        "a5_stub( 'FadeIn' ) a5_stub( 'FadeOut' ) a5_stub( 'CameraLock' ) a5_stub( 'CameraSequence' ) a5_stub( 'CameraSetClipping' )\n"
        "a5_stub( 'AddHints' ) a5_stub( 'ShowHint' ) a5_stub( 'ShowLeaveZoneDialog' ) a5_stub( 'ShowLoseDialog' )\n"
        "a5_stub( 'BeginZone' ) a5_stub( 'LeaveToSubZone' ) a5_stub( 'SetLeaveZoneMode' ) a5_stub( 'SetFirstMissionMode' ) a5_stub( 'SetTutorialMode' )\n"
        "a5_stub( 'StartGameEx' ) a5_stub( 'StartGameWithSequence' ) a5_stub( 'DelayGameStartEx' ) a5_stub( 'DividedDeployDemo' )\n"
        "a5_stub( 'SetTimeOfDay' ) a5_stub( 'SetMaxCriticalSeverity' ) a5_stub( 'SlowSyncAIMap' ) a5_stub( 'WantTurnBased' ) a5_stub( 'PlayerGiveTurn' )\n"
        "a5_stub( 'ScenarioAddGoal' ) a5_stub( 'ScenarioClueGive' ) a5_stub( 'ScenarioSetGoalComplete' ) a5_stub( 'ScenarioSetTaskComplete' )\n"
        "a5_stub( 'ButtonCreateState' ) a5_stub( 'CreateWindow' ) a5_stub( 'GetWindow' ) a5_stub( 'IsUIActionIDPresent', 0 )\n"
        "a5_stub( 'CreateAndActivateItem' ) a5_stub( 'DestroyItemInHand' ) a5_stub( 'FindItem' ) a5_stub( 'ItemSetToWaypoint' ) a5_stub( 'ItemUnload' )\n"
        "a5_stub( 'ObjectGetDestroyStage', 0 ) a5_stub( 'ObjectGetHP', 100 ) a5_stub( 'ObjectLockDoor' ) a5_stub( 'ObjectUnlockDoor' ) a5_stub( 'ObjectRestoreFromPocket' )\n"
        "a5_stub( 'PassCalcerIsActive', 0 ) a5_stub( 'GetScenarioNumber', 0 ) a5_stub( 'GetUnitPK' ) a5_stub( 'PlayerGetUnitsEx' )\n"
        "a5_stub( 'HasInventoryItemGroup', 0 ) a5_stub( 'HasInventoryItemUnit', 0 )\n"
        "a5_stub( 'UnitApplyTableCritical' ) a5_stub( 'UnitAttackWaypoint' ) a5_stub( 'UnitCreateItem' ) a5_stub( 'UnitDrawWeapon' ) a5_stub( 'UnitFlyToWaypoint' )\n"
        "a5_stub( 'UnitGetSkill', 0 ) a5_stub( 'UnitGetSkillMaxValue', 0 ) a5_stub( 'UnitGetToHitUnit', 0 ) a5_stub( 'UnitGetToHitWaypoint', 0 )\n"
        "a5_stub( 'UnitGiveRandomPerks' ) a5_stub( 'UnitGrenadeToUnit' ) a5_stub( 'UnitHealCriticals' ) a5_stub( 'UnitHoldItem' ) a5_stub( 'UnitInArea', 0 )\n"
        "a5_stub( 'UnitIsCarryingCorpse', 0 ) a5_stub( 'UnitIsHearUnit', 0 ) a5_stub( 'UnitIsUsingCannon', 0 ) a5_stub( 'UnitIsWeaponInHand', 0 ) a5_stub( 'UnitIsWearingPK', 0 )\n"
        "a5_stub( 'UnitKeepMoving' ) a5_stub( 'UnitLeavePK' ) a5_stub( 'UnitLockPose' ) a5_stub( 'UnitRegenerateVP' )\n"
        "a5_stub( 'UnitSetCivilianLogic' ) a5_stub( 'UnitSetFearLogic' ) a5_stub( 'UnitSetGuardLogic' ) a5_stub( 'UnitSetHideProbability' ) a5_stub( 'UnitSetNormalLogic' )\n"
        "a5_stub( 'UnitSetPanicLogic' ) a5_stub( 'UnitSetRetreatLogic' ) a5_stub( 'UnitSetScriptLogic' ) a5_stub( 'UnitSetSkill' ) a5_stub( 'UnitSetSkillMaxValue' ) a5_stub( 'UnitSetToHit' )\n"
        "a5_stub( 'UnitShootPrepare' ) a5_stub( 'UnitStop' ) a5_stub( 'UnitSwitchToGrenade' ) a5_stub( 'UnitTakeItem' ) a5_stub( 'UnitTakeObject' ) a5_stub( 'UnitWearPK' )\n"
        "out( '[android] script prelude loaded\\n' )\n";
}
