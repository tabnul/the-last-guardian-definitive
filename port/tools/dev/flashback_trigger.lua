-- Headless trigger for the shield flashback (s04_FirstAntenna FA_a04_AntennaRoom_001 -> demo fa_040 -> s19_FlashBack1).
-- Use: TLG_LUA_INJECT=<this file>, TLG_LUA_INJECT_SCENE=FA_a04_AntennaRoom, save CUSA03627_2026-10-20_before_shield.
-- The mission script starts fa_040 when (1) Trico did the antenna-threat edge action in the excite animation, (2) the boy calls Trico
-- (3 s later the step flag advances) and (3) the boy is in volume boy_in_antennaRoom_FA_a04. These three answers are forced for this
-- room only; the game's own script then plays the cutscene. Then FA_a04_ExcitingTrico_001 waits for Trico to hold the boy in his mouth
-- (Ai2_Condor_GetProps('holding_player_flag')): forced too, until the flashback's demo fl1_010 starts. Every original comes back.
local orig = {
  vol = MS.HasObjectInVolumeWithFlag,
  edge = MS.CondorCompareSpEdgeAction,
  anim = MS.CondorCompareAnimationState,
  call = Ai2.PlayerUtils.IsCallingCondor,
  demo = MS.DemoPlay,
  props = MS.Ai2_Condor_GetProps,
}
local function restore()
  MS.HasObjectInVolumeWithFlag = orig.vol
  MS.CondorCompareSpEdgeAction = orig.edge
  MS.CondorCompareAnimationState = orig.anim
  Ai2.PlayerUtils.IsCallingCondor = orig.call
  L('flashback trigger: room checks restored')
end
MS.HasObjectInVolumeWithFlag = function(v, ...)
  if v == 'boy_in_antennaRoom_FA_a04' then return true end
  return orig.vol(v, ...)
end
MS.CondorCompareSpEdgeAction = function(a, ...)
  if a == 'FA_antenna_threat' then return true end
  return orig.edge(a, ...)
end
MS.CondorCompareAnimationState = function(s, ...)
  if s == 'fa_035_excite' then return true end
  return orig.anim(s, ...)
end
Ai2.PlayerUtils.IsCallingCondor = function(...) return true end
local holding = true
MS.Ai2_Condor_GetProps = function(name, ...)
  -- only in the step that waits for it (MSC_GetCurrentState: the mission step's name)
  if holding and name == 'holding_player_flag' and MSC_GetCurrentState() == 'FA_a04_ExcitingTrico_001' then return true end
  return orig.props(name, ...)
end
MS.DemoPlay = function(path, step, ...)
  if path == 'game/stages/s04_FirstAntenna/demo/fa_040' and step == 'action_1' then
    L('flashback trigger: fa_040 action_1')
    restore()
  elseif path == 'game/stages/s19_FlashBack1/demo/fl1_010' then
    holding = false
    MS.Ai2_Condor_GetProps = orig.props
    MS.DemoPlay = orig.demo
    L('flashback trigger: flashback started, all originals restored')
  end
  return orig.demo(path, step, ...)
end
L('flashback trigger armed')
