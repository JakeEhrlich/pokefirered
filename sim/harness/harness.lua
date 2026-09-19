-- mGBA Lua bridge for the battle-simulator cross-check (loaded from harness_gen.lua, which prepends
-- the address table A). Runs a TCP server; the host driver (sim/build/crosscheck) connects to it.
--
-- Protocol (newline-terminated ASCII lines):
--   Lua -> host : READY
--   host -> Lua : START <flags> <trainer> <seed> <terrain> <badges> <style> <sceneOff> <engineRngSeed> <hex player party> [<scriptOpponent> <hex enemy party>]
--   Lua -> host : STARTED
--   Lua -> host : REQ <kind> <battler> <seq> / RNGCALLS <engine> <other> / MONS <hex> / ST3 <hex> / SIDE <hex>
--                 / DIS <hex> / WEATHER <hex> / WISH <hex> / PPARTY <hex> / EPARTY <hex> / MISC <hex> / END
--   host -> Lua : ACT <type> <moveSlot> <target> <partySlot> <item>
--   Lua -> host : DONE <outcome> / RNG ... / (same snapshot lines) / END

local H = A.gSimHarness
local OFF = { go = 4, flags = 8, trainer = 0xC, seed = 0xE, terrain = 0x10, badges = 0x11, style = 0x12, sceneOff = 0x13,
              state = 0x14, outcome = 0x18, request = 0x20, reqBattler = 0x24, reqSeq = 0x28, ansSeq = 0x2C,
              ansType = 0x30, ansMove = 0x31, ansTarget = 0x32, ansParty = 0x33, ansItem = 0x34,
              engineRngState = 0x38, engineRngCalls = 0x3C, otherRngCalls = 0x40, rangeCount = 0x44, ranges = 0x48, callerRing = 0xC8,
              scriptOpponent = 0x1C8, useEnemyParty = 0x1C9 }

local client = nil
local rxbuf = ""
local idleStatePath = "/tmp/simharness_idle_8899.ss"
local haveIdleState = false
local pendingStart = nil
local pendingSince = nil
local stableKey = nil
local stableFrames = 0
local lastSeqSent = 0
local doneSent = true
local inBattle = false

local function hex(s)
  return (s:gsub(".", function(c) return string.format("%02x", c:byte()) end))
end

local function send(line)
  if client then client:send(line .. "\n") end
end

local function sendSnapshot()
  send(string.format("RNGCALLS %d %d", emu:read32(H + OFF.engineRngCalls), emu:read32(H + OFF.otherRngCalls)))
  send("CALLERS " .. hex(emu:readRange(A.gSimHarness + OFF.callerRing, 256)))
  send("MONS " .. hex(emu:readRange(A.gBattleMons, 88 * 4)))
  send("ST3 " .. hex(emu:readRange(A.gStatuses3, 16)))
  send("SIDE " .. hex(emu:readRange(A.gSideStatuses, 4)) .. hex(emu:readRange(A.gSideTimers, 24)))
  send("DIS " .. hex(emu:readRange(A.gDisableStructs, 28 * 4)))
  send("WEATHER " .. hex(emu:readRange(A.gBattleWeather, 2)))
  send("WISH " .. hex(emu:readRange(A.gWishFutureKnock, 44)))
  send("PPARTY " .. hex(emu:readRange(A.gPlayerParty, 600)))
  send("EPARTY " .. hex(emu:readRange(A.gEnemyParty, 600)))
  send("MISC " .. hex(emu:readRange(A.gBattlerPartyIndexes, 8)) .. hex(emu:readRange(A.gAbsentBattlerFlags, 1)) .. hex(emu:readRange(A.gBattleOutcome, 1)))
  send("END")
end

local function applyStart(p)
  for i = 0, #p.party / 2 - 1 do
    emu:write8(A.gPlayerParty + i, tonumber(p.party:sub(i * 2 + 1, i * 2 + 2), 16))
  end
  emu:write32(H + OFF.flags, p.flags)
  emu:write16(H + OFF.trainer, p.trainer)
  emu:write16(H + OFF.seed, p.seed)
  emu:write8(H + OFF.terrain, p.terrain)
  emu:write8(H + OFF.badges, p.badges)
  emu:write8(H + OFF.style, p.style)
  emu:write8(H + OFF.sceneOff, p.sceneOff)
  emu:write32(H + OFF.rangeCount, #ENGINE_RANGES)
  for i, r in ipairs(ENGINE_RANGES) do
    emu:write32(H + OFF.ranges + (i - 1) * 8, r[1])
    emu:write32(H + OFF.ranges + (i - 1) * 8 + 4, r[2])
  end
  emu:write32(H + OFF.engineRngState, p.rngSeed)
  emu:write8(H + OFF.scriptOpponent, p.scriptOpponent)
  if p.enemyParty and #p.enemyParty >= 1200 then
    for i = 0, #p.enemyParty / 2 - 1 do
      emu:write8(A.gEnemyParty + i, tonumber(p.enemyParty:sub(i * 2 + 1, i * 2 + 2), 16))
    end
    emu:write8(H + OFF.useEnemyParty, 1)
  else
    emu:write8(H + OFF.useEnemyParty, 0)
  end
  lastSeqSent = emu:read32(H + OFF.reqSeq)
  doneSent = false
  inBattle = true
  emu:write32(H + OFF.go, 1)
  send("STARTED")
end

local function onFrame()
  if emu:read32(H) ~= 0x53494D48 then return end
  local state = emu:read32(H + OFF.state)
  if not haveIdleState and state == 0 and emu:read32(H + OFF.go) == 0 then
    -- Remember the pristine idle state so every battle can start from exactly the same place.
    emu:saveStateFile(idleStatePath)
    haveIdleState = true
    console:log("idle save state written to " .. idleStatePath)
  end
  if not client then return end
  if pendingStart then
    if state ~= 1 then
      local p = pendingStart
      pendingStart = nil
      applyStart(p)
    end
    return
  end
  if not inBattle then return end
  -- tap A every other frame so battle text never waits for the player
  if (emu:currentFrame() % 2) == 0 then emu:addKey(0) else emu:clearKeys(1) end
  local state = emu:read32(H + OFF.state)
  if state == 2 and not doneSent then
    doneSent = true
    inBattle = false
    emu:clearKeys(1)
    send("DONE " .. emu:read32(H + OFF.outcome))
    sendSnapshot()
    return
  end
  local req = emu:read32(H + OFF.request)
  local seq = emu:read32(H + OFF.reqSeq)
  if req ~= 0 and seq ~= lastSeqSent then
    -- The other battlers' controllers (the AI) may still be finishing their decisions over the
    -- next few frames (in doubles the partner's AI runs several frames after the request);
    -- snapshot only once the request has been pending for a few frames AND the engine state
    -- (compared regions, RNG counts, controller state machine) has not changed for 3 frames,
    -- which mirrors the simulator stepping to quiescence before it reports a request.
    if pendingSince == nil then pendingSince = emu:currentFrame(); stableKey = nil; stableFrames = 0 end
    local key = emu:readRange(A.gBattleMons, 88 * 4) .. emu:readRange(A.gBattleCommunication, 8)
      .. emu:readRange(A.gBattleControllerExecFlags, 4) .. emu:readRange(H + OFF.engineRngCalls, 4)
      .. emu:readRange(A.gPlayerParty, 600) .. emu:readRange(A.gEnemyParty, 600)
      .. emu:readRange(A.gDisableStructs, 28 * 4) .. emu:readRange(A.gWishFutureKnock, 44)
    if key == stableKey then stableFrames = stableFrames + 1 else stableKey = key; stableFrames = 0 end
    local waited = emu:currentFrame() - pendingSince
    if waited >= 4 and (stableFrames >= 3 or waited >= 240) then
      if waited >= 240 then console:log("warning: state never settled after request; snapshotting anyway") end
      pendingSince = nil
      lastSeqSent = seq
      send(string.format("REQ %d %d %d", req, emu:read32(H + OFF.reqBattler), seq))
      sendSnapshot()
    end
  else
    pendingSince = nil
  end
end

local function handleLine(line)
  local args = {}
  for w in line:gmatch("%S+") do args[#args + 1] = w end
  if args[1] == "START" then
    local p = { flags = tonumber(args[2]), trainer = tonumber(args[3]), seed = tonumber(args[4]), terrain = tonumber(args[5]),
                badges = tonumber(args[6]), style = tonumber(args[7]), sceneOff = tonumber(args[8]), rngSeed = tonumber(args[9]),
                party = args[10], scriptOpponent = tonumber(args[11] or "0"), enemyParty = args[12] }
    inBattle = false
    if haveIdleState then
      emu:loadStateFile(idleStatePath)   -- back to the idle loop even if a previous battle was abandoned
    end
    pendingStart = p                     -- applied on the next frame, once the ROM sits in the idle loop
  elseif args[1] == "ACT" then
    emu:write8(H + OFF.ansType, tonumber(args[2]))
    emu:write8(H + OFF.ansMove, tonumber(args[3]))
    emu:write8(H + OFF.ansTarget, tonumber(args[4]))
    emu:write8(H + OFF.ansParty, tonumber(args[5]))
    emu:write16(H + OFF.ansItem, tonumber(args[6]))
    emu:write32(H + OFF.ansSeq, emu:read32(H + OFF.reqSeq))
  elseif args[1] == "PING" then
    send("PONG")
  end
end

local function onReceived()
  while true do
    local p, err = client:receive(4096)
    if p then
      rxbuf = rxbuf .. p
      while true do
        local nl = rxbuf:find("\n", 1, true)
        if not nl then break end
        local line = rxbuf:sub(1, nl - 1)
        rxbuf = rxbuf:sub(nl + 1)
        handleLine(line)
      end
    else
      if err ~= socket.ERRORS.AGAIN then
        console:log("client disconnected: " .. tostring(err))
        client:close()
        client = nil
      end
      return
    end
  end
end

local function onAccept()
  local sock, err = server:accept()
  if err then console:error("accept: " .. tostring(err)) return end
  if client then client:close() end
  client = sock
  rxbuf = ""
  client:add("received", onReceived)
  client:add("error", function() client = nil end)
  console:log("crosscheck host connected")
  send("READY")
end

callbacks:add("frame", onFrame)

-- Several mGBA instances can run at once: each bridge takes the first free port from 8899 upwards and
-- keeps its own idle save state.
local port = 8899
while true do
  server, err = socket.bind(nil, port)
  if not err then break end
  if err == socket.ERRORS.ADDRESS_IN_USE and port < 8899 + 16 then
    port = port + 1
  else
    console:error("bind: " .. tostring(err))
    server = nil
    break
  end
end
if server then
  idleStatePath = "/tmp/simharness_idle_" .. port .. ".ss"
  local ok
  ok, err = server:listen()
  if err then console:error("listen: " .. tostring(err)) else
    console:log("battle-sim harness listening on port " .. port)
    server:add("received", onAccept)
  end
end
