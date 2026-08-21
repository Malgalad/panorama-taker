local MOD_VERSION = "0.1.19"

local function log(message)
    print("[PanoramaCaptureProbe] " .. message)
end

log("reload verification marker v" .. MOD_VERSION)

local environmentSequence = nil
local productionSession = nil
local pauseEventState = nil
local hudRehideFrames = 0
local bridgeSessionCounter = 0

-- User-editable capture settings. Reload CET after changing them.
local captureConfig = {
    overlap = 0.08,
    adaptiveYawGuard = 0.05,
    settleSeconds = 1.5,
    calibrationFrames = 2,
    pitchToleranceDegrees = 0.25,
    maxPitchCorrections = 3,
    -- Set true after installing PanoramaCaptureReShade.addon64.
    automatedScreenshots = true,
    bridgeDirectory = ".",
}

log("bridge directory: " .. captureConfig.bridgeDirectory)

local function isFiniteNumber(value)
    return type(value) == "number" and value == value and value ~= math.huge and value ~= -math.huge
end

local function configurationError()
    if not isFiniteNumber(captureConfig.overlap) or captureConfig.overlap < 0 or captureConfig.overlap >= 0.5 then
        return "overlap must be a finite number in [0, 0.5)"
    end
    if not isFiniteNumber(captureConfig.adaptiveYawGuard) or captureConfig.adaptiveYawGuard < 0 or
        captureConfig.adaptiveYawGuard >= 0.25 then
        return "adaptiveYawGuard must be a finite number in [0, 0.25)"
    end
    if not isFiniteNumber(captureConfig.settleSeconds) or captureConfig.settleSeconds < 0 or
        captureConfig.settleSeconds > 60 then
        return "settleSeconds must be a finite number in [0, 60]"
    end
    if not isFiniteNumber(captureConfig.calibrationFrames) or captureConfig.calibrationFrames < 1 or
        captureConfig.calibrationFrames % 1 ~= 0 then
        return "calibrationFrames must be a positive integer"
    end
    if not isFiniteNumber(captureConfig.pitchToleranceDegrees) or captureConfig.pitchToleranceDegrees <= 0 or
        captureConfig.pitchToleranceDegrees > 10 then
        return "pitchToleranceDegrees must be a finite number in (0, 10]"
    end
    if not isFiniteNumber(captureConfig.maxPitchCorrections) or captureConfig.maxPitchCorrections < 0 or
        captureConfig.maxPitchCorrections % 1 ~= 0 then
        return "maxPitchCorrections must be a non-negative integer"
    end
    if type(captureConfig.automatedScreenshots) ~= "boolean" then
        return "automatedScreenshots must be true or false"
    end
    if type(captureConfig.bridgeDirectory) ~= "string" or captureConfig.bridgeDirectory == "" then
        return "bridgeDirectory must be a non-empty path"
    end
    return nil
end

local function bridgeFile(name)
    return captureConfig.bridgeDirectory .. "/PanoramaCaptureBridge." .. name
end

local function writeBridgeRequest(sessionId, poseIndex, token)
    local temporary = bridgeFile("request.tmp")
    local requestPath = bridgeFile("request")
    local output, openError = io.open(temporary, "wb")
    if output == nil then
        return false, "cannot open bridge request: " .. tostring(openError)
    end
    output:write(string.format("1\t%s\t%d\t%s\n", sessionId, poseIndex, token))
    output:close()
    os.remove(requestPath)
    local renamed, renameError = os.rename(temporary, requestPath)
    if not renamed then
        os.remove(temporary)
        return false, "cannot publish bridge request: " .. tostring(renameError)
    end
    return true
end

local function readBridgeAck()
    local ackPath = bridgeFile("ack")
    local input = io.open(ackPath, "rb")
    if input == nil then
        return nil
    end
    local line = input:read("*l")
    input:close()
    os.remove(ackPath)
    if line == nil then
        return nil
    end
    local version, sessionId, poseIndex, token, path = line:match("^([^\t]+)\t([^\t]+)\t([^\t]+)\t([^\t]+)\t(.*)$")
    if version ~= "1" or sessionId == nil or poseIndex == nil or token == nil or path == nil then
        return nil
    end
    return { sessionId = sessionId, poseIndex = tonumber(poseIndex), token = token, path = path }
end

local function jsonEscape(value)
    return tostring(value):gsub("[\\\"\n\r\t]", function(character)
        local escapes = { ["\\"] = "\\\\", ["\""] = "\\\"", ["\n"] = "\\n",
            ["\r"] = "\\r", ["\t"] = "\\t" }
        return escapes[character]
    end)
end

local function vectorJson(vector)
    return string.format("[%.9f,%.9f,%.9f]", vector.x, vector.y, vector.z)
end

local function writeSessionMetadata(session, state)
    local temporary = session.metadataPath .. ".tmp"
    local output = io.open(temporary, "wb")
    if output == nil then
        log("Metadata warning: cannot open " .. session.metadataPath)
        return false
    end
    output:write("{\n", string.format("  \"schema_version\":1,\n  \"session_id\":\"%s\",\n",
        jsonEscape(session.sessionId)))
    output:write(string.format("  \"horizontal_fov_deg\":%.9f,\n  \"vertical_fov_deg\":%.9f,\n",
        session.horizontalFov, session.verticalFov))
    output:write(string.format("  \"state\":\"%s\",\n  \"poses\":[\n", jsonEscape(state)))
    for index, record in ipairs(session.metadataRecords) do
        output:write(string.format(
            "    {\"index\":%d,\"row\":%d,\"column\":%d,\"commanded_yaw_deg\":%.9f," ..
            "\"commanded_pitch_deg\":%.9f,\"observed_pitch_deg\":%.9f,\"forward\":%s," ..
            "\"right\":%s,\"up\":%s,\"settle_seconds\":%.6f,\"screenshot_path\":\"%s\"}%s\n",
            record.index, record.row, record.column, record.commandedYaw, record.commandedPitch,
            record.observedPitch, vectorJson(record.forward), vectorJson(record.right),
            vectorJson(record.up), record.settleSeconds, jsonEscape(record.screenshotPath),
            index < #session.metadataRecords and "," or ""))
    end
    output:write("  ]\n}\n")
    output:close()
    os.remove(session.metadataPath)
    local renamed = os.rename(temporary, session.metadataPath)
    if not renamed then
        os.remove(temporary)
        log("Metadata warning: cannot publish " .. session.metadataPath)
        return false
    end
    return true
end

local function effectiveFov()
    local cameraSystem = Game.GetCameraSystem()
    if cameraSystem == nil then
        return nil, nil, "camera system unavailable"
    end
    local okFov, vertical = pcall(function() return cameraSystem:GetActiveCameraFOV() end)
    local okAspect, aspect = pcall(function() return cameraSystem:GetAspectRatio() end)
    if not okFov or type(vertical) ~= "number" or vertical <= 0 or vertical >= 180 then
        return nil, nil, "active vertical FoV unavailable"
    end
    if not okAspect or type(aspect) ~= "number" or aspect <= 0 then
        return nil, nil, "active aspect ratio unavailable"
    end
    local horizontal = math.deg(2.0 * math.atan(math.tan(math.rad(vertical) / 2.0) * aspect))
    if horizontal <= 0 or horizontal >= 180 then
        return nil, nil, "derived horizontal FoV is invalid"
    end
    return horizontal, vertical, nil
end

local function buildFullSpherePlan(horizontal, vertical, overlap, adaptiveYawGuard)
    if overlap < 0 or overlap >= 1 or adaptiveYawGuard < 0 or adaptiveYawGuard >= 1 then
        return nil, "overlap must be in [0, 1)"
    end
    local yawStep = horizontal * (1.0 - overlap)
    local guardedYawStep = yawStep * (1.0 - adaptiveYawGuard)
    local pitchStep = vertical * (1.0 - overlap)
    local rows = math.max(1, math.ceil((180.0 - vertical) / pitchStep) + 1)
    local polarGuard = vertical * overlap / 2.0
    local maxPitch = math.min(89.0, math.max(0.0, 90.0 - vertical / 2.0 + polarGuard))
    local plan = {}
    local maximumColumns = 0
    for row = 0, rows - 1 do
        local pitch = rows == 1 and 0.0 or -maxPitch + (2.0 * maxPitch * row / (rows - 1))
        local columns = math.max(1,
            math.ceil(360.0 * math.cos(math.rad(math.abs(pitch))) / guardedYawStep))
        maximumColumns = math.max(maximumColumns, columns)
        for column = 0, columns - 1 do
            plan[#plan + 1] = { row = row, column = column, columns = columns,
                yaw = column * 360.0 / columns, pitch = pitch }
        end
    end
    return {
        horizontalFov = horizontal,
        verticalFov = vertical,
        yawStep = yawStep,
        guardedYawStep = guardedYawStep,
        adaptiveYawGuard = adaptiveYawGuard,
        pitchStep = pitchStep,
        columns = maximumColumns,
        rows = rows,
        poses = plan,
    }, nil
end

local function componentClassName(component)
    local ok, className = pcall(function() return component:GetClassName() end)
    return ok and tostring(className) or ""
end

local function entityHash(entity)
    local okId, entityId = pcall(function() return entity:GetEntityID() end)
    if not okId or entityId == nil then
        return nil
    end
    local okHash, hash = pcall(function() return entityId.hash end)
    return okHash and tostring(hash) or nil
end

local function componentKey(entity, component)
    local owner = entityHash(entity) or tostring(entity)
    local className = componentClassName(component)
    local okName, name = pcall(function() return component:GetName() end)
    if okName and name ~= nil then
        return owner .. ":" .. className .. ":" .. tostring(name)
    end
    return owner .. ":" .. className .. ":" .. tostring(component)
end

local function widgetKey(widget)
    local okName, name = pcall(function() return widget:GetName() end)
    if okName and name ~= nil then
        return tostring(name)
    end
    return tostring(widget)
end

local function activeWeapon(player)
    local ok, weapon = pcall(function()
        local equipmentSystem = Game.GetScriptableSystemsContainer():Get(CName.new("EquipmentSystem"))
        return equipmentSystem and equipmentSystem:GetActiveWeaponObject(player, 40) or nil
    end)
    return ok and weapon or nil
end

local function captureMeshEntities(player)
    local entities = { player }
    local weapon = activeWeapon(player)
    if weapon ~= nil then
        entities[#entities + 1] = weapon
    end
    return entities
end

local function snapshotCaptureMeshes(player)
    local meshes = {}
    for _, entity in ipairs(captureMeshEntities(player)) do
        local components = entity:GetComponents()
        for _, component in ipairs(components) do
            if string.find(componentClassName(component), "Mesh") then
                local okEnabled, enabled = pcall(function() return component:IsEnabled() end)
                if not okEnabled then
                    return nil, "mesh enabled-state getter unavailable"
                end
                meshes[#meshes + 1] = {
                    component = component,
                    key = componentKey(entity, component),
                    enabled = enabled,
                }
            end
        end
    end
    return meshes
end

local function hideCaptureMeshes(environment)
    local player = Game.GetPlayer()
    if player == nil or entityHash(player) ~= environment.playerHash then
        return false, "player changed during environment probe"
    end
    for _, entity in ipairs(captureMeshEntities(player)) do
        local components = entity:GetComponents()
        for _, component in ipairs(components) do
            if string.find(componentClassName(component), "Mesh") then
                local key = componentKey(entity, component)
                local found = false
                for _, saved in ipairs(environment.meshes) do
                    if saved.key == key then
                        found = true
                        break
                    end
                end
                if not found then
                    local okEnabled, enabled = pcall(function() return component:IsEnabled() end)
                    if not okEnabled then
                        return false, "new mesh enabled-state getter unavailable"
                    end
                    environment.meshes[#environment.meshes + 1] = {
                        component = component,
                        key = key,
                        enabled = enabled,
                    }
                end
                local okToggle, toggleError = pcall(function() component:Toggle(false) end)
                if not okToggle then
                    return false, "mesh hide failed: " .. tostring(toggleError)
                end
            end
        end
    end
    return true
end

local function restoreCaptureMeshes(environment)
    for _, saved in ipairs(environment.meshes) do
        pcall(function() saved.component:Toggle(saved.enabled) end)
    end
end

local function snapshotHud()
    local inkSystem = Game.GetInkSystem()
    if inkSystem == nil then
        return nil, "ink system unavailable"
    end
    local layer = inkSystem:GetLayer(CName.new("inkHUDLayer"))
    if layer == nil then
        return nil, "HUD layer unavailable"
    end
    local window = layer:GetVirtualWindow()
    if window == nil then
        return nil, "HUD virtual window unavailable"
    end
    local widgets = {}
    local okCount, count = pcall(function() return window:GetNumChildren() end)
    if not okCount or type(count) ~= "number" then
        return nil, "HUD virtual window is not an ink compound widget"
    end
    for i = 0, count - 1 do
        local widget = window:GetWidgetByIndex(i)
        if widget ~= nil then
            local okOpacity, opacity = pcall(function() return widget:GetOpacity() end)
            if not okOpacity then
                return nil, "HUD opacity getter unavailable"
            end
            widgets[#widgets + 1] = {
                widget = widget,
                key = widgetKey(widget),
                opacity = opacity,
            }
        end
    end
    return { window = window, widgets = widgets }, nil
end

local function currentHudWindow()
    local ok, window = pcall(function()
        local inkSystem = Game.GetInkSystem()
        local layer = inkSystem:GetLayer(CName.new("inkHUDLayer"))
        return layer:GetVirtualWindow()
    end)
    return ok and window or nil
end

local function hideHud(environment)
    local window = currentHudWindow() or environment.hud.window
    environment.hud.window = window
    local okCount, count = pcall(function() return window:GetNumChildren() end)
    if not okCount or type(count) ~= "number" then
        return false, "HUD virtual window is not an ink compound widget"
    end
    for i = 0, count - 1 do
        local widget = window:GetWidgetByIndex(i)
        if widget ~= nil then
            local key = widgetKey(widget)
            local found = false
            for _, saved in ipairs(environment.hud.widgets) do
                if saved.key == key then
                    found = true
                    break
                end
            end
            if not found then
                local okOpacity, opacity = pcall(function() return widget:GetOpacity() end)
                if not okOpacity then
                    return false, "new HUD opacity getter unavailable"
                end
                environment.hud.widgets[#environment.hud.widgets + 1] = {
                    widget = widget,
                    key = key,
                    opacity = opacity,
                }
            end
            local okOpacity, opacityError = pcall(function() widget:SetOpacity(0.0) end)
            if not okOpacity then
                return false, "HUD hide failed: " .. tostring(opacityError)
            end
        end
    end
    return true
end

local function restoreHud(environment)
    local restored = {}
    local window = currentHudWindow() or environment.hud.window
    local okCount, count = pcall(function() return window:GetNumChildren() end)
    if okCount and type(count) == "number" then
        for i = 0, count - 1 do
            local widget = window:GetWidgetByIndex(i)
            if widget ~= nil then
                local key = widgetKey(widget)
                for _, saved in ipairs(environment.hud.widgets) do
                    if saved.key == key then
                        local okRestore, restoreError = pcall(function()
                            widget:SetOpacity(saved.opacity)
                        end)
                        if not okRestore then
                            log("HUD restore failed for " .. key .. ": " .. tostring(restoreError))
                        end
                        restored[saved.key] = true
                        break
                    end
                end
            end
        end
    else
        log("HUD restore: current virtual window is not an ink compound widget")
    end
    for _, saved in ipairs(environment.hud.widgets) do
        if not restored[saved.key] then
            local okRestore, restoreError = pcall(function()
                saved.widget:SetOpacity(saved.opacity)
            end)
            if not okRestore then
                log("HUD saved-widget restore failed for " .. saved.key .. ": " .. tostring(restoreError))
            else
                log("HUD widget not present in current layer; attempted saved handle restore for " .. saved.key)
            end
        end
    end
end

local function hasConflictingTimeDilation()
    local timeSystem = Game.GetTimeSystem()
    if timeSystem == nil then
        return true
    end
    for _, reason in ipairs({ "console", "consoleCommand", "pause", "radial" }) do
        local ok, active = pcall(function() return timeSystem:IsTimeDilationActive(reason) end)
        if ok and active then
            log("Environment probe: conflicting time dilation is active for " .. reason)
            return true
        end
    end
    return false
end

local inputRestrictions = {
    "GameplayRestriction.NoMovement",
    "GameplayRestriction.NoCameraControl",
}

local function applyInputRestrictions(environment)
    environment.inputRestrictions = {}
    local player = Game.GetPlayer()
    local statusEffects = Game.GetStatusEffectSystem()
    if player == nil or statusEffects == nil then
        return false, "input restriction system unavailable"
    end
    for _, effectName in ipairs(inputRestrictions) do
        local ok, effectError = pcall(function()
            statusEffects:ApplyStatusEffect(player:GetEntityID(), TweakDBID.new(effectName),
                player:GetRecordID(), player:GetEntityID())
        end)
        if not ok then
            return false, "input restriction failed: " .. tostring(effectError)
        end
        environment.inputRestrictions[#environment.inputRestrictions + 1] = effectName
    end
    return true
end

local function restoreInputRestrictions(environment)
    if environment.inputRestrictions == nil then
        return
    end
    local player = environment.player
    local statusEffects = Game.GetStatusEffectSystem()
    if player == nil or statusEffects == nil then
        return
    end
    for _, effectName in ipairs(environment.inputRestrictions) do
        pcall(function()
            statusEffects:RemoveStatusEffect(player:GetEntityID(), TweakDBID.new(effectName), 999)
        end)
    end
end

local function applyEnvironmentControls(environment)
    if hasConflictingTimeDilation() then
        return false, "conflicting time dilation is active"
    end
    local timeSystem = Game.GetTimeSystem()
    local okTime, timeError = pcall(function()
        timeSystem:SetIgnoreTimeDilationOnLocalPlayerZero(true)
        timeSystem:SetTimeDilation("PanoramaCapture", 0.0000000000001)
    end)
    if not okTime then
        return false, "time freeze failed: " .. tostring(timeError)
    end
    environment.timeApplied = true

    local inputLocked, inputError = applyInputRestrictions(environment)
    if not inputLocked then
        return false, inputError
    end

    local okHud, hudError = hideHud(environment)
    if not okHud then
        return false, hudError
    end
    local okMeshes, meshError = hideCaptureMeshes(environment)
    if not okMeshes then
        return false, meshError
    end
    return true
end

local function restoreEnvironmentControls(environment)
    hudRehideFrames = 0
    restoreInputRestrictions(environment)
    restoreCaptureMeshes(environment)
    restoreHud(environment)
    if environment.timeApplied then
        pcall(function()
            local timeSystem = Game.GetTimeSystem()
            timeSystem:UnsetTimeDilation("PanoramaCapture")
            timeSystem:SetIgnoreTimeDilationOnLocalPlayerZero(false)
        end)
    end
end

local function photoModeActive()
    local ok, active = pcall(function()
        local definitions = Game.GetAllBlackboardDefs()
        local blackboard = Game.GetBlackboardSystem():Get(definitions.PhotoMode)
        return blackboard:GetBool(definitions.PhotoMode.IsActive)
    end)
    return ok and active == true
end

local function captureBlockReason()
    if pauseEventState == true then
        return "game UI reports a paused menu"
    end
    local okPaused, paused = pcall(function()
        return Game.GetSystemRequestsHandler():IsGamePaused()
    end)
    if okPaused and paused == true then
        return "game is paused"
    end

    local okMounted, mounted = pcall(function()
        local definitions = Game.GetAllBlackboardDefs()
        local blackboard = Game.GetBlackboardSystem():Get(definitions.UI_ActiveVehicleData)
        return blackboard:GetBool(definitions.UI_ActiveVehicleData.IsPlayerMounted)
    end)
    if okMounted and mounted == true then
        return "player is mounted in a vehicle"
    end
    return nil
end

local function observedCameraMetadata()
    local cameraSystem = Game.GetCameraSystem()
    if cameraSystem == nil then
        return nil, "camera system unavailable"
    end
    local ok, forward, right, up, fov, aspect = pcall(function()
        return cameraSystem:GetActiveCameraForward(), cameraSystem:GetActiveCameraRight(),
            cameraSystem:GetActiveCameraUp(), cameraSystem:GetActiveCameraFOV(),
            cameraSystem:GetAspectRatio()
    end)
    if not ok or forward == nil or right == nil or up == nil or type(fov) ~= "number" or
        type(aspect) ~= "number" or fov <= 0 or aspect <= 0 then
        return nil, "active camera readback unavailable"
    end
    local horizontal = math.deg(2.0 * math.atan(math.tan(math.rad(fov) / 2.0) * aspect))
    local function norm(v)
        return math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z)
    end
    local function dot(a, b)
        return a.x * b.x + a.y * b.y + a.z * b.z
    end
    local basisValid = math.abs(norm(forward) - 1.0) < 0.02 and
        math.abs(norm(right) - 1.0) < 0.02 and math.abs(norm(up) - 1.0) < 0.02 and
        math.abs(dot(forward, right)) < 0.02 and math.abs(dot(forward, up)) < 0.02 and
        math.abs(dot(right, up)) < 0.02
    local clampedForwardZ = math.max(-1.0, math.min(1.0, forward.z))
    return {
        forward = forward,
        right = right,
        up = up,
        horizontalFov = horizontal,
        verticalFov = fov,
        pitch = math.deg(math.asin(clampedForwardZ)),
        basisValid = basisValid,
    }, nil
end

local function logPoseMetadata(session, pose, observed)
    local f = observed.forward
    local r = observed.right
    local u = observed.up
    log(string.format(
        "POSE_METADATA index=%d/%d row=%d column=%d commanded_yaw=%.6f commanded_pitch=%.6f " ..
        "observed_forward=(%.9f,%.9f,%.9f) observed_right=(%.9f,%.9f,%.9f) " ..
        "observed_up=(%.9f,%.9f,%.9f) hfov=%.9f vfov=%.9f settle_seconds=%.6f " ..
        "basis_valid=%s observed_pitch=%.6f",
        session.index, #session.plan.poses, pose.row, pose.column, pose.yaw, pose.pitch,
        f.x, f.y, f.z, r.x, r.y, r.z, u.x, u.y, u.z, observed.horizontalFov,
        observed.verticalFov, session.lastSettleSeconds, tostring(observed.basisValid), observed.pitch))
    return {
        index = session.index,
        row = pose.row,
        column = pose.column,
        commandedYaw = pose.yaw,
        commandedPitch = pose.pitch,
        observedPitch = observed.pitch,
        forward = observed.forward,
        right = observed.right,
        up = observed.up,
        settleSeconds = session.lastSettleSeconds,
    }
end

local function applyPose(snapshot, yawDegrees)
    Game.GetTeleportationFacility():Teleport(snapshot.player, snapshot.position,
        EulerAngles.new(0, 0, yawDegrees))
    snapshot.waitFrames = 2
end

registerInput("panorama_capture_probe_environment", "Panorama: probe FPP capture environment", function(down)
    if not down then
        return
    end

    if environmentSequence ~= nil then
        environmentSequence.restoreRequested = true
        log("Environment probe: restore queued; release the camera after the next update.")
        return
    end
    local configError = configurationError()
    if configError ~= nil then
        log("Environment probe cancelled: invalid configuration: " .. configError .. ".")
        return
    end
    local blockReason = captureBlockReason()
    if blockReason ~= nil then
        log("Environment probe cancelled: " .. blockReason .. ".")
        return
    end
    if photoModeActive() then
        log("Environment probe requires normal FPP view; exit Photo Mode first.")
        return
    end

    local player = Game.GetPlayer()
    local cameraSystem = Game.GetCameraSystem()
    local camera = player and player:GetFPPCameraComponent()
    local playerHash = player and entityHash(player)
    if player == nil or playerHash == nil or cameraSystem == nil or camera == nil then
        log("Environment probe requires an active player, camera system, and FPP camera.")
        return
    end

    local hud, hudError = snapshotHud()
    if hud == nil then
        log("Environment probe cancelled: " .. tostring(hudError))
        return
    end
    local meshes, meshError = snapshotCaptureMeshes(player)
    if meshes == nil then
        log("Environment probe cancelled: " .. tostring(meshError))
        return
    end
    local okOrientation, orientation = pcall(function() return camera:GetLocalOrientation() end)
    if not okOrientation or orientation == nil then
        log("Environment probe cancelled: FPP local orientation unavailable.")
        return
    end
    local position = player:GetWorldPosition()
    environmentSequence = {
        camera = camera,
        player = player,
        playerHash = playerHash,
        position = Vector4.new(position.x, position.y, position.z, position.w),
        originalOrientation = orientation,
        originalYaw = player:GetWorldYaw(),
        hud = hud,
        meshes = meshes,
        state = "applying",
        waitFrames = 0,
        settleElapsed = 0.0,
        settleSecondsRequired = captureConfig.settleSeconds,
        targetYaw = 45.0,
        targetPitch = 15.0,
        timeApplied = false,
        restoreRequested = false,
    }
    local okControls, controlError = applyEnvironmentControls(environmentSequence)
    if not okControls then
        restoreEnvironmentControls(environmentSequence)
        environmentSequence = nil
        log("Environment probe cancelled: " .. tostring(controlError))
        return
    end
    applyPose(environmentSequence, environmentSequence.originalYaw + environmentSequence.targetYaw)
    environmentSequence.state = "rotated_pending"
    log("Environment probe active; press the same hotkey to restore.")
end)

local function startProductionSession()
    if productionSession ~= nil or environmentSequence ~= nil then
        log("Production session already active.")
        return
    end
    local configError = configurationError()
    if configError ~= nil then
        log("Production session cancelled: invalid configuration: " .. configError .. ".")
        return
    end
    local blockReason = captureBlockReason()
    if blockReason ~= nil then
        log("Production session cancelled: " .. blockReason .. ".")
        return
    end
    if photoModeActive() then
        log("Production session requires normal FPP view; exit Photo Mode first.")
        return
    end
    local horizontal, vertical, fovError = effectiveFov()
    if horizontal == nil then
        log("Production session cancelled: " .. tostring(fovError))
        return
    end
    local plan, planError = buildFullSpherePlan(
        horizontal, vertical, captureConfig.overlap, captureConfig.adaptiveYawGuard)
    if plan == nil then
        log("Production session cancelled: " .. tostring(planError))
        return
    end
    local player = Game.GetPlayer()
    local cameraSystem = Game.GetCameraSystem()
    local camera = player and player:GetFPPCameraComponent()
    local playerHash = player and entityHash(player)
    if player == nil or playerHash == nil or cameraSystem == nil or camera == nil then
        log("Production session requires an active player, camera system, and FPP camera.")
        return
    end
    local hud, hudError = snapshotHud()
    if hud == nil then
        log("Production session cancelled: " .. tostring(hudError))
        return
    end
    local meshes, meshError = snapshotCaptureMeshes(player)
    if meshes == nil then
        log("Production session cancelled: " .. tostring(meshError))
        return
    end
    local okOrientation, orientation = pcall(function() return camera:GetLocalOrientation() end)
    if not okOrientation or orientation == nil then
        log("Production session cancelled: FPP local orientation unavailable.")
        return
    end
    local firstPose = plan.poses[1]
    environmentSequence = {
        camera = camera,
        player = player,
        playerHash = playerHash,
        position = Vector4.new(player:GetWorldPosition().x, player:GetWorldPosition().y,
            player:GetWorldPosition().z, player:GetWorldPosition().w),
        originalOrientation = orientation,
        originalYaw = player:GetWorldYaw(),
        hud = hud,
        meshes = meshes,
        state = "applying",
        waitFrames = 0,
        settleElapsed = 0.0,
        settleSecondsRequired = captureConfig.settleSeconds,
        targetYaw = firstPose.yaw,
        targetPitch = firstPose.pitch,
        pitchCommand = firstPose.pitch,
        pitchCorrections = 0,
        pitchCorrectionPending = false,
        timeApplied = false,
        restoreRequested = false,
        production = true,
    }
    local okControls, controlError = applyEnvironmentControls(environmentSequence)
    if not okControls then
        restoreEnvironmentControls(environmentSequence)
        environmentSequence = nil
        log("Production session cancelled: " .. tostring(controlError))
        return
    end
    bridgeSessionCounter = bridgeSessionCounter + 1
    local sessionId = string.format("%d-%d", os.time(), bridgeSessionCounter)
    productionSession = {
        plan = plan,
        index = 1,
        sessionId = sessionId,
        horizontalFov = horizontal,
        verticalFov = vertical,
        lastSettleSeconds = 0.0,
        pitchCorrection = 0.0,
        awaitingScreenshot = false,
        metadataPath = bridgeFile("pano-" .. sessionId .. ".json"),
        metadataRecords = {},
        pendingMetadata = nil,
    }
    writeSessionMetadata(productionSession, "active")
    applyPose(environmentSequence, environmentSequence.originalYaw + firstPose.yaw)
    environmentSequence.state = "pitch_calibration_pending"
    log(string.format("Production session started: %d poses, %dx%d, HFoV=%.3f VFoV=%.3f, adaptive yaw guard=%.1f%%.",
        #plan.poses, plan.columns, plan.rows, horizontal, vertical, plan.adaptiveYawGuard * 100.0))
end

local function queueNextProductionPose()
    if productionSession == nil or environmentSequence == nil then
        return
    end
    if productionSession.index >= #productionSession.plan.poses then
        environmentSequence.restoreRequested = true
        log("Production session: final screenshot acknowledged; restoration queued.")
        return
    end
    productionSession.index = productionSession.index + 1
    local pose = productionSession.plan.poses[productionSession.index]
    environmentSequence.targetYaw = pose.yaw
    environmentSequence.targetPitch = pose.pitch
    environmentSequence.pitchCommand = pose.pitch + productionSession.pitchCorrection
    environmentSequence.pitchCorrections = 0
    environmentSequence.pitchCorrectionPending = false
    environmentSequence.settleElapsed = 0.0
    environmentSequence.state = "rotated_pending"
    applyPose(environmentSequence, environmentSequence.originalYaw + pose.yaw)
    log(string.format("Production pose %d/%d queued: row=%d column=%d yaw=%.3f pitch=%.3f.",
        productionSession.index, #productionSession.plan.poses,
        pose.row, pose.column, pose.yaw, pose.pitch))
end

registerInput("panorama_capture_start", "Panorama: start full-sphere pose session", function(down)
    if down then
        startProductionSession()
    end
end)

registerInput("panorama_capture_advance", "Panorama: advance full-sphere pose", function(down)
    if not down then
        return
    end
    if productionSession == nil or environmentSequence == nil then
        log("No production session is active.")
        return
    end
    if captureConfig.automatedScreenshots then
        log("Automated screenshot mode is active; ReShade acknowledgement advances the pose.")
        return
    end
    if environmentSequence.state ~= "active" then
        log("Production pose is not settled; wait for the ready log.")
        return
    end
    queueNextProductionPose()
end)

registerInput("panorama_capture_abort", "Panorama: abort full-sphere pose session", function(down)
    if down and environmentSequence ~= nil then
        os.remove(bridgeFile("request"))
        os.remove(bridgeFile("ack"))
        environmentSequence.restoreRequested = true
        log("Production session: abort queued.")
    end
end)

local function reportStatus()
    local configError = configurationError()
    if configError ~= nil then
        log("Status: invalid configuration: " .. configError .. ".")
        return
    end
    local horizontal, vertical, fovError = effectiveFov()
    local fovStatus = fovError and ("FoV unavailable: " .. fovError) or
        string.format("HFoV=%.3f VFoV=%.3f", horizontal, vertical)
    if environmentSequence == nil then
        log(string.format(
            "Status: idle; %s; overlap=%.1f%% settle=%.2fs.",
            fovStatus, captureConfig.overlap * 100.0, captureConfig.settleSeconds))
        return
    end

    local sequence = environmentSequence
    local poseStatus = "probe"
    if productionSession ~= nil then
        poseStatus = string.format("pose=%d/%d", productionSession.index, #productionSession.plan.poses)
    end
    local remainingSeconds = math.max(0, (sequence.settleSecondsRequired or 0) - (sequence.settleElapsed or 0.0))
    log(string.format(
        "Status: active state=%s %s; %s; settle_remaining=%.2fs.",
        sequence.state, poseStatus, fovStatus, remainingSeconds))
end

registerInput("panorama_capture_status", "Panorama: report capture status", function(down)
    if down then
        reportStatus()
    end
end)

local function abortEnvironment(reason)
    if environmentSequence == nil then
        return
    end
    os.remove(bridgeFile("request"))
    os.remove(bridgeFile("ack"))
    if productionSession ~= nil then
        writeSessionMetadata(productionSession, "aborted")
    end
    restoreEnvironmentControls(environmentSequence)
    environmentSequence = nil
    productionSession = nil
    log("Environment probe: aborted (" .. reason .. ").")
end

local function restorePoseImmediately(environment)
    pcall(function()
        Game.GetTeleportationFacility():Teleport(environment.player, environment.position,
            EulerAngles.new(0, 0, environment.originalYaw))
    end)
    pcall(function()
        environment.camera:SetLocalOrientation(environment.originalOrientation)
    end)
end

registerForEvent("onUpdate", function(deltaTime)
    if environmentSequence ~= nil then
        if hudRehideFrames > 0 then
            if not environmentSequence.restoreRequested then
                hideHud(environmentSequence)
            end
            hudRehideFrames = hudRehideFrames - 1
        end
        local currentPlayer = Game.GetPlayer()
        if currentPlayer == nil or entityHash(currentPlayer) ~= environmentSequence.playerHash then
            abortEnvironment("player changed or became unavailable during session transition")
            return
        end
        if productionSession ~= nil and productionSession.awaitingScreenshot then
            local acknowledgement = readBridgeAck()
            if acknowledgement ~= nil then
                local poseMatches = acknowledgement.poseIndex == productionSession.index
                local sessionMatches = acknowledgement.sessionId == productionSession.sessionId
                local tokenMatches = acknowledgement.token == string.format("pano-%s-%03d",
                    productionSession.sessionId, productionSession.index)
                if poseMatches and sessionMatches and tokenMatches then
                    productionSession.awaitingScreenshot = false
                    if acknowledgement.path:sub(1, 6) == "ERROR:" then
                        log("Production session cancelled: ReShade bridge error " .. acknowledgement.path)
                        writeSessionMetadata(productionSession, "failed")
                        environmentSequence.restoreRequested = true
                    else
                        local metadata = productionSession.pendingMetadata
                        if metadata ~= nil then
                            metadata.screenshotPath = acknowledgement.path
                            productionSession.metadataRecords[#productionSession.metadataRecords + 1] = metadata
                            productionSession.pendingMetadata = nil
                        end
                        local completed = productionSession.index >= #productionSession.plan.poses
                        writeSessionMetadata(productionSession, completed and "completed" or "active")
                        log(string.format("Production screenshot acknowledged: pose %d/%d path=%s",
                            productionSession.index, #productionSession.plan.poses, acknowledgement.path))
                        queueNextProductionPose()
                    end
                else
                    log("Production screenshot acknowledgement ignored: request identity mismatch.")
                end
            end
        end
        if environmentSequence.restoreRequested and environmentSequence.state ~= "restore_pending" and
            environmentSequence.state ~= "restore_correcting" then
            applyPose(environmentSequence, environmentSequence.originalYaw)
            environmentSequence.state = "restore_pending"
            environmentSequence.waitFrames = 2
        elseif environmentSequence.waitFrames > 0 then
            environmentSequence.waitFrames = environmentSequence.waitFrames - 1
        elseif environmentSequence.state == "pitch_calibration_pending" then
            environmentSequence.camera:SetLocalOrientation(Quaternion.new(0, 0, 0, 1))
            environmentSequence.state = "pitch_calibration_observing"
            environmentSequence.waitFrames = captureConfig.calibrationFrames
        elseif environmentSequence.state == "pitch_calibration_observing" then
            local observed, observedError = observedCameraMetadata()
            if observed == nil or not observed.basisValid then
                log("Production session cancelled: initial pitch calibration failed: " ..
                    tostring(observedError or "active camera basis is not orthonormal"))
                environmentSequence.restoreRequested = true
            else
                productionSession.pitchCorrection = -observed.pitch
                environmentSequence.pitchCommand = environmentSequence.targetPitch +
                    productionSession.pitchCorrection
                environmentSequence.state = "rotated_pending"
                log(string.format(
                    "Production pitch calibration: zero-command observed=%.3f; offset=%.3f.",
                    observed.pitch, productionSession.pitchCorrection))
            end
        elseif environmentSequence.state == "rotated_pending" then
            environmentSequence.camera:SetLocalOrientation(Quaternion.new(
                math.sin(math.rad(environmentSequence.pitchCommand or environmentSequence.targetPitch) / 2.0),
                0, 0,
                math.cos(math.rad(environmentSequence.pitchCommand or environmentSequence.targetPitch) / 2.0)))
            local hidden, hideError = hideHud(environmentSequence)
            local meshesHidden, meshError = hideCaptureMeshes(environmentSequence)
            if not hidden or not meshesHidden then
                log("Environment probe: transition re-hide failed: " .. tostring(hideError or meshError))
            end
            environmentSequence.state = "settling"
            environmentSequence.settleElapsed = 0.0
            environmentSequence.settleSecondsRequired = captureConfig.settleSeconds
        elseif environmentSequence.state == "settling" then
            if type(deltaTime) == "number" and deltaTime > 0 then
                environmentSequence.settleElapsed = environmentSequence.settleElapsed + deltaTime
            end
            if environmentSequence.settleElapsed >= environmentSequence.settleSecondsRequired then
                if environmentSequence.production then
                    local observed, observedError = observedCameraMetadata()
                    if observed == nil then
                        log("Production session cancelled: " .. tostring(observedError))
                        environmentSequence.restoreRequested = true
                    elseif not observed.basisValid then
                        log("Production session cancelled: active camera basis is not orthonormal")
                        environmentSequence.restoreRequested = true
                    else
                        productionSession.lastSettleSeconds = environmentSequence.settleElapsed
                        local pose = productionSession.plan.poses[productionSession.index]
                        local pitchError = environmentSequence.targetPitch - observed.pitch
                        if math.abs(pitchError) > captureConfig.pitchToleranceDegrees then
                            if environmentSequence.pitchCorrections >= captureConfig.maxPitchCorrections then
                                log(string.format(
                                    "Production session cancelled: pose %d pitch error %.3f exceeds %.3f tolerance after %d corrections.",
                                    productionSession.index, pitchError,
                                    captureConfig.pitchToleranceDegrees,
                                    environmentSequence.pitchCorrections))
                                environmentSequence.restoreRequested = true
                                environmentSequence.state = "active"
                            else
                                environmentSequence.pitchCorrections =
                                    environmentSequence.pitchCorrections + 1
                                productionSession.pitchCorrection =
                                    productionSession.pitchCorrection + pitchError
                                environmentSequence.pitchCommand = environmentSequence.targetPitch +
                                    productionSession.pitchCorrection
                                environmentSequence.pitchCorrectionPending = true
                                environmentSequence.state = "rotated_pending"
                                log(string.format(
                                    "Production pose %d pitch correction %d/%d: observed=%.3f target=%.3f command=%.3f.",
                                    productionSession.index, environmentSequence.pitchCorrections,
                                    captureConfig.maxPitchCorrections, observed.pitch,
                                    environmentSequence.targetPitch, environmentSequence.pitchCommand))
                            end
                        elseif environmentSequence.pitchCorrectionPending then
                            environmentSequence.pitchCorrectionPending = false
                            environmentSequence.settleSecondsRequired = captureConfig.settleSeconds
                            environmentSequence.settleElapsed = 0.0
                            environmentSequence.state = "settling"
                        else
                            productionSession.pendingMetadata = logPoseMetadata(productionSession, pose, observed)
                            if captureConfig.automatedScreenshots then
                                local token = string.format("pano-%s-%03d", productionSession.sessionId,
                                    productionSession.index)
                                local requestOk, requestError = writeBridgeRequest(
                                    productionSession.sessionId, productionSession.index, token)
                                if not requestOk then
                                    log("Production session cancelled: " .. tostring(requestError))
                                    environmentSequence.restoreRequested = true
                                else
                                    productionSession.awaitingScreenshot = true
                                    environmentSequence.state = "awaiting_screenshot"
                                    log(string.format(
                                        "Production pose ready: %d/%d; ReShade screenshot requested.",
                                        productionSession.index, #productionSession.plan.poses))
                                end
                            else
                                log(string.format(
                                    "Production pose ready: %d/%d; capture screenshot, then advance.",
                                    productionSession.index, #productionSession.plan.poses))
                                environmentSequence.state = "active"
                            end
                        end
                    end
                else
                    log(string.format("Environment probe settled: settle_seconds=%.3f",
                        environmentSequence.settleElapsed))
                    environmentSequence.state = "active"
                end
            end
        elseif environmentSequence.state == "restore_pending" then
            environmentSequence.camera:SetLocalOrientation(environmentSequence.originalOrientation)
            environmentSequence.waitFrames = 2
            environmentSequence.state = "restore_correcting"
        elseif environmentSequence.state == "restore_correcting" then
            if productionSession ~= nil then
                local completed = productionSession.index >= #productionSession.plan.poses and
                    productionSession.pendingMetadata == nil
                writeSessionMetadata(productionSession, completed and "completed" or "aborted")
            end
            restoreEnvironmentControls(environmentSequence)
            environmentSequence = nil
            productionSession = nil
            log("Environment probe: complete.")
        end
    end

end)

registerForEvent("onInit", function()
    Observe("gameuiPopupsManager", "OnMenuUpdate", function(_, isInMenu)
        pauseEventState = isInMenu == true
        if environmentSequence ~= nil then
            hudRehideFrames = 3
        end
    end)
    Observe("gameuiPhotoModeMenuController", "OnShow", function()
        pauseEventState = true
        if environmentSequence ~= nil then
            hudRehideFrames = 3
        end
    end)
    Observe("gameuiPhotoModeMenuController", "OnHide", function()
        pauseEventState = false
        if environmentSequence ~= nil then
            hudRehideFrames = 3
        end
    end)
    Observe("PlayerPuppet", "OnDetach", function()
        abortEnvironment("player detached or save transition started")
    end)
end)

registerForEvent("onOverlayClose", function()
    if environmentSequence ~= nil and not environmentSequence.restoreRequested then
        hudRehideFrames = 5
    end
end)

registerForEvent("onShutdown", function()
    if environmentSequence ~= nil then
        restorePoseImmediately(environmentSequence)
        abortEnvironment("CET shutdown/reload")
    end
end)
