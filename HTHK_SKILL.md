# HẢI TẶC TRUYỀN KỲ — Game Mod Reference

Package: `com.hhkb.rpg.gp`  
Engine: **Unity IL2CPP + toLua hybrid** (`libil2cpp.so` + `libtolua.so`)  
Verified: 2026-10-20

## Repo Files

- `main_hthk_lgl.cpp` — LGL May Root version
- `main_hthk_hamod.cpp` — HAMOD framework version
- `menu_hthk.js` — HAMOD menu config

## 3 Features (Menu Index)

| index | type | name | impl |
|-------|------|------|------|
| 0 | SeekBar (max 99999) | Sát thương ×1-99999 | Lua hook `GetHurtVal` (libtolua) |
| 1 | SeekBar (max 3) | Tốc độ ×3/5/10 | IL2CPP hook `Time.set_timeScale` + `get_timeScale` |
| 2 | Toggle | Bỏ quảng cáo | Lua hook Advertisement.sc_advideo_begin + patch KLLSDK.AD |

## Lua Module Hooks (libtolua.so)

Hook point: `luaL_loadbufferx` in `libtolua.so`  
Chunkname format: `@path/to/module.lua`

### Damage Module

```
chunkname: @battle/skill/base/object/attackResultData.lua
class: AttackResultData
method: GetHurtVal() — getter, returns number (damage value)
```

**Logic**: Multiply `GetHurtVal` return value by `f0` (seekbar 0, range 1–99999).  
**Team check**: Only multiply when `self:GetMaster():GetTeamId() == 1` AND `self:GetTarget():GetTeamId() ~= 1`  
(attacker is player team, defender is not — prevents buffing enemies in PvP defense)

### Ads Module

```
chunkname: @game/module/advertisement/Advertisement.lua  (ALSO lowercase)
chunkname: @game/module/advertisement/advertisement.lua  (both are loaded!)
key: sc_advideo_begin — server reply handler, called after server grants ad_uid
```

**Ad flow (server handshake)**:
1. `cs_advideo_begin` → server → `sc_advideo_begin(result=OK, ad_uid=X)`
2. Inside `sc_advideo_begin`: `AD:SetRewardAdParam` → `AD:ShowRewardAd`
3. SDK fires: `OnRewardAdOpened` → `OnAdRewardSuccessCallBack` → `OnRewardAdClosed`
4. `OnRewardAdClosed` triggers `cs_advideo_over` with correct `ad_uid`

**KLLSDK.AD** — global Lua object (SDK wrapper):
- `ShowRewardAd` — shows actual video. **This is what we replace.**
- `IsInitRewardAd` — gate check before begin
- `IsRewardAdAvailable` — gate check before begin
- `SetRewardAdParam` — SDK init (may crash if SDK not inited, wrap in pcall)
- Callbacks: `OnRewardAdOpened`, `OnAdRewardSuccessCallBack`, `OnRewardAdClosed`

**Correct skip approach** — patch `KLLSDK.AD.ShowRewardAd` lazily (inside sc_advideo_begin intercept):

```lua
local function patch_ad_sdk()
  local AD = KLLSDK and KLLSDK.AD
  if type(AD) ~= 'table' then return end
  if AD._hthk then return end  -- guard: prevent double-patch
  AD._hthk = true
  local _init = AD.IsInitRewardAd
  AD.IsInitRewardAd = function(...) if ads_on() then return true end return _init and _init(...) end
  local _avail = AD.IsRewardAdAvailable
  AD.IsRewardAdAvailable = function(...) if ads_on() then return true end return _avail and _avail(...) end
  local _param = AD.SetRewardAdParam
  AD.SetRewardAdParam = function(...) if ads_on() then pcall(_param, ...) return end return _param and _param(...) end
  local _show = AD.ShowRewardAd
  AD.ShowRewardAd = function(self, ...)
    if ads_on() then
      if type(AD.OnRewardAdOpened) == 'function' then pcall(function() AD:OnRewardAdOpened() end) end
      if type(AD.OnAdRewardSuccessCallBack) == 'function' then pcall(function() AD:OnAdRewardSuccessCallBack() end) end
      if type(AD.OnRewardAdClosed) == 'function' then pcall(function() AD:OnRewardAdClosed() end) end
      return true
    end
    return _show(self, ...)
  end
end

-- Intercept sc_advideo_begin: patch AD then let original run (ad_uid handshake works)
local _sc_begin = M.sc_advideo_begin
if type(_sc_begin) == 'function' then
  M.sc_advideo_begin = function(self, ...)
    if ads_on() then patch_ad_sdk() end
    return _sc_begin(self, ...)
  end
end
```

**WHY this order**: `_sc_begin(self, ...)` still runs → server processes begin→over correctly with real `ad_uid`. Only `ShowRewardAd` is replaced so video never plays.

**WHY NOT GetExempt branch**: The `GetExempt` (ad-exempt privilege) skips `cs_advideo_begin` entirely, sending bare `cs_advideo_over` without a valid `ad_uid` → server rejects with "ad_uid is synchronous".

**Double-load guard**: Both uppercase and lowercase Advertisement.lua are loaded. The `AD._hthk = true` flag ensures only the first one patches.

## Speed Hook (libil2cpp.so)

```
Namespace: UnityEngine
Class: Time
Methods: get_timeScale() → float, set_timeScale(float) → void
```

**Logic**: Multiply game's own timeScale by speed multiplier. Do NOT overwrite — game may set timeScale=0 for pause.

```
SPD_MUL[4] = {1.0f, 3.0f, 5.0f, 10.0f}
index 0 = ×1 (off), index 1 = ×3, index 2 = ×5, index 3 = ×10
```

## HAMOD Config

File: `config_com.hhkb.rpg.gp.txt` (7 space-separated floats)  
Search paths: `mods/config_com.hhkb.rpg.gp.txt` then `config_com.hhkb.rpg.gp.txt`

```
f0 = damage multiplier (1.0 - 99999.0)
f1 = speed index (0.0 - 3.0)
f2 = ads skip (0.0 or 1.0)
```

## Khi Game Update

1. Kiểm tra `luaL_loadbufferx` còn trong `libtolua.so` (thường không đổi)
2. Kiểm tra chunkname damage: `@battle/skill/base/object/attackResultData.lua` (case-sensitive)
3. Kiểm tra chunkname ads: `@game/module/advertisement/Advertisement.lua` (cả lowercase)
4. Kiểm tra `KLLSDK.AD` còn là global object với các method trên
5. Kiểm tra `sc_advideo_begin` còn là key trong module trả về của Advertisement.lua
6. Kiểm tra IL2CPP: `UnityEngine.Time.set_timeScale` và `get_timeScale` (thường stable)

Nếu chunkname đổi: log TẤT CẢ chunkname khi hook để tìm cái mới.
