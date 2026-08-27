local MOD_VERSION = "0.1.65"
local DEVELOPMENT_MODE = false

local function log(message)
    print("[PanoramaCaptureProbe] " .. message)
end

local function devLog(message)
    if DEVELOPMENT_MODE then
        log(message)
    end
end

local function registerDevelopmentInput(name, description, callback)
    if DEVELOPMENT_MODE then
        registerInput(name, description, callback)
    end
end

log("reload verification marker v" .. MOD_VERSION)

local environmentSequence = nil
local productionSession = nil
local destroyStandaloneCamera
local pauseEventState = nil
local hudRehideFrames = 0
local bridgeSessionCounter = 0
local nativeSettings = nil
local settingsDraftSettle = nil
local settingsDraftScreenshotCooldown = nil
local settingsDraftCaptureFov = nil
local nativeSettingsCaptureSummaryOption = nil
local updateCaptureSummary = nil
local bridgeCaptureRange = "unknown"
local bridgeStatusPollElapsed = 0.0
local SETTINGS_FILE = "settings.json"
local SETTINGS_DEFAULTS = {
    settleSeconds = 1.0,
    screenshotCooldownSeconds = 3.1,
}

local function isFiniteNumber(value)
    return type(value) == "number" and value == value and value ~= math.huge and value ~= -math.huge
end

-- Native Settings values are persisted in settings.json beside this script.
local captureConfig = {
    overlap = 0.08,
    adaptiveYawGuard = 0.05,
    settleSeconds = SETTINGS_DEFAULTS.settleSeconds,
    -- nil uses the active in-game display FoV for this session.
    captureFov = nil,
    screenshotCooldownSeconds = SETTINGS_DEFAULTS.screenshotCooldownSeconds,
    screenshotAckTimeoutSeconds = 15.0,
    -- Release captures require true after installing PanoramaCaptureReShade.addon64.
    automatedScreenshots = true,
    bridgeDirectory = ".",
}
local STANDALONE_CAMERA_PATH = "base\\entities\\cameras\\simple_free_camera.ent"
local CAMERA_SPAWN_TIMEOUT_SECONDS = 3.0
local INITIAL_EXPOSURE_HOLD_SECONDS = 0.1
local CAMERA_POSITION_TOLERANCE = 0.001
local CAMERA_FOV_TOLERANCE_DEGREES = 0.05
local CAMERA_PITCH_TOLERANCE_DEGREES = 0.25
local CAMERA_YAW_TOLERANCE_DEGREES = 0.25


local function savePersistentSettings()
    local settings = {
        schemaVersion = 1,
        settleSeconds = captureConfig.settleSeconds,
        screenshotCooldownSeconds = captureConfig.screenshotCooldownSeconds,
    }
    if captureConfig.captureFov ~= nil then
        settings.captureFov = captureConfig.captureFov
    end
    local okEncode, encoded = pcall(json.encode, settings)
    if not okEncode then
        log("Settings save failed: " .. tostring(encoded))
        return false
    end
    local file, openError = io.open(SETTINGS_FILE, "w")
    if file == nil then
        log("Settings save failed: " .. tostring(openError))
        return false
    end
    local writeResult, writeError = file:write(encoded)
    local closeResult, closeError = file:close()
    if writeResult == nil or closeResult == nil then
        log("Settings save failed: " .. tostring(writeError or closeError))
        return false
    end
    return true
end

local function loadPersistentSettings()
    local file = io.open(SETTINGS_FILE, "r")
    if file == nil then
        return
    end
    local content = file:read("*a")
    file:close()
    local okDecode, settings = pcall(json.decode, content)
    if not okDecode or type(settings) ~= "table" then
        log("Settings load ignored: settings.json is not valid JSON.")
        return
    end
    if isFiniteNumber(settings.settleSeconds) and settings.settleSeconds >= 0.1 and settings.settleSeconds <= 3.0 then
        captureConfig.settleSeconds = settings.settleSeconds
    end
    if isFiniteNumber(settings.screenshotCooldownSeconds) and settings.screenshotCooldownSeconds >= 0 and
        settings.screenshotCooldownSeconds <= 5.0 then
        captureConfig.screenshotCooldownSeconds = settings.screenshotCooldownSeconds
    end
    if isFiniteNumber(settings.captureFov) and
        settings.captureFov >= 30 and settings.captureFov <= 120 then
        captureConfig.captureFov = settings.captureFov
    end
end

local function nativeSettingsCaptureFovDefault()
    local cameraSystem = Game.GetCameraSystem()
    local ok, vertical, aspect = pcall(function()
        return cameraSystem:GetActiveCameraFOV(), cameraSystem:GetAspectRatio()
    end)
    if ok and isFiniteNumber(vertical) and isFiniteNumber(aspect) and
        vertical > 0 and vertical < 180 and aspect > 0 then
        return math.deg(2.0 * math.atan(math.tan(math.rad(vertical) / 2.0) * aspect))
    end
    return 90.0
end

local function registerNativeSettings()
    local ok, settings = pcall(function() return GetMod("nativeSettings") end)
    if not ok or settings == nil then
        return
    end
    nativeSettings = settings
    settingsDraftSettle = captureConfig.settleSeconds
    settingsDraftScreenshotCooldown = captureConfig.screenshotCooldownSeconds
    settingsDraftCaptureFov = captureConfig.captureFov
    local visibleCaptureFov = settingsDraftCaptureFov or nativeSettingsCaptureFovDefault()
    local okApi = pcall(function()
        nativeSettings.addTab("/PanoramaCapture", "Panorama Capture", nil)
        nativeSettings.addSubcategory("/PanoramaCapture/Capture", "Capture", 1)
        nativeSettings.addRangeFloat(
            "/PanoramaCapture/Capture", "Settling delay",
            "Wait for temporal accumulation after a verified pose before taking a screenshot. Path tracing needs this most; other modes may use less.",
            0.1, 3.0, 0.1, "%.1fs", settingsDraftSettle, SETTINGS_DEFAULTS.settleSeconds,
            function(value)
                settingsDraftSettle = value
                captureConfig.settleSeconds = value
                savePersistentSettings()
                if updateCaptureSummary ~= nil then
                    updateCaptureSummary()
                end
            end, 1)
        nativeSettings.addRangeFloat(
            "/PanoramaCapture/Capture", "ReShade toast cooldown",
            "Wait for ReShade's screenshot toast before the next capture. Set to 0 when ReShade screenshot notifications are disabled.",
            0.0, 5.0, 0.1, "%.1fs", settingsDraftScreenshotCooldown,
            SETTINGS_DEFAULTS.screenshotCooldownSeconds,
            function(value)
                settingsDraftScreenshotCooldown = value
                captureConfig.screenshotCooldownSeconds = value
                savePersistentSettings()
                if updateCaptureSummary ~= nil then
                    updateCaptureSummary()
                end
        end, 2)
        nativeSettings.addRangeFloat(
                "/PanoramaCapture/Capture", "Capture FoV",
                "Optional display FoV override. Until changed, capture uses the active in-game FoV.",
                30.0, 120.0, 1.0, "%.0f°", visibleCaptureFov, visibleCaptureFov,
                function(value)
                    settingsDraftCaptureFov = value
                    captureConfig.captureFov = value
                    savePersistentSettings()
                    if updateCaptureSummary ~= nil then
                        updateCaptureSummary()
                    end
                end, 3)
        nativeSettingsCaptureSummaryOption = nativeSettings.addCustom(
            "/PanoramaCapture/Capture", function(parent, option)
                local text = inkText.new()
                text:SetName("panoramaCaptureSummary")
                text:SetFontFamily("base\\gameplay\\gui\\fonts\\raj\\raj.inkfontfamily")
                text:SetFontStyle("Medium")
                text:SetFontSize(36)
                text:SetLetterCase(textLetterCase.OriginalCase)
                text:SetTintColor(HDRColor.new({ Red = 0.94088, Green = 0.30472, Blue = 0.27808, Alpha = 1.0 }))
                text:SetHorizontalAlignment(textHorizontalAlignment.Left)
                text:SetVerticalAlignment(textVerticalAlignment.Center)
                text:SetMargin(inkMargin.new({ left = 40.0, top = 6.0, right = 0.0, bottom = 6.0 }))
                text:SetText(option.text or "Capture estimate loading…")
                text:SetInteractive(false)
                text:Reparent(parent, -1)
                option.textWidget = text
            end, 4)
        if updateCaptureSummary ~= nil then
            updateCaptureSummary()
        end
        nativeSettings.registerRestoreDefaultsCallback("/PanoramaCapture/Capture", false, function()
            settingsDraftSettle = SETTINGS_DEFAULTS.settleSeconds
            settingsDraftScreenshotCooldown = SETTINGS_DEFAULTS.screenshotCooldownSeconds
            captureConfig.settleSeconds = SETTINGS_DEFAULTS.settleSeconds
            captureConfig.screenshotCooldownSeconds = SETTINGS_DEFAULTS.screenshotCooldownSeconds
            settingsDraftCaptureFov = nil
            captureConfig.captureFov = nil
            savePersistentSettings()
            if updateCaptureSummary ~= nil then
                updateCaptureSummary()
            end
        end)
    end)
    if not okApi then
        nativeSettings = nil
    end
end

devLog("bridge directory: " .. captureConfig.bridgeDirectory)

local function syncNativeSettings()
    if settingsDraftSettle ~= nil then
        captureConfig.settleSeconds = settingsDraftSettle
    end
    if settingsDraftScreenshotCooldown ~= nil then
        captureConfig.screenshotCooldownSeconds = settingsDraftScreenshotCooldown
    end
    if settingsDraftCaptureFov ~= nil then
        captureConfig.captureFov = settingsDraftCaptureFov
    end
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
    if captureConfig.captureFov ~= nil and (not isFiniteNumber(captureConfig.captureFov) or
        captureConfig.captureFov < 30 or captureConfig.captureFov > 120) then
        return "captureFov must be a finite number in [30, 120]"
    end
    if not isFiniteNumber(captureConfig.screenshotCooldownSeconds) or
        captureConfig.screenshotCooldownSeconds < 0 or captureConfig.screenshotCooldownSeconds > 60 then
        return "screenshotCooldownSeconds must be a finite number in [0, 60]"
    end
    if not isFiniteNumber(captureConfig.screenshotAckTimeoutSeconds) or
        captureConfig.screenshotAckTimeoutSeconds < 1 or captureConfig.screenshotAckTimeoutSeconds > 60 then
        return "screenshotAckTimeoutSeconds must be a finite number in [1, 60]"
    end
    if not DEVELOPMENT_MODE and captureConfig.automatedScreenshots ~= true then
        return "automatedScreenshots must remain true; manual mode is development-only"
    end
    if type(captureConfig.bridgeDirectory) ~= "string" or captureConfig.bridgeDirectory == "" then
        return "bridgeDirectory must be a non-empty path"
    end
    return nil
end

local function bridgeFile(name)
    return captureConfig.bridgeDirectory .. "/PanoramaCaptureBridge." .. name
end

local function refreshBridgeCaptureRange()
    local input = io.open(bridgeFile("status"), "rb")
    if input == nil then
        bridgeCaptureRange = "unknown"
        return
    end
    local line = input:read("*l")
    input:close()
    if line == nil then
        bridgeCaptureRange = "unknown"
        return
    end
    local version, captureRange = line:match("^([^\t]+)\t([^\t]+)$")
    if version == "1" and (captureRange == "hdr" or captureRange == "sdr") then
        bridgeCaptureRange = captureRange
    else
        bridgeCaptureRange = "unknown"
    end
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

local function numberArrayJson(values, count)
    if type(values) ~= "table" then
        return nil
    end
    local result = {}
    for index = 1, count do
        if not isFiniteNumber(values[index]) then
            return nil
        end
        result[index] = string.format("%.9f", values[index])
    end
    return "[" .. table.concat(result, ",") .. "]"
end

local function cameraDiagnosticCall(cameraSystem, methodName)
    local ok, value = pcall(function() return cameraSystem[methodName](cameraSystem) end)
    return ok and value or nil
end

local function validVector(value)
    local ok, x, y, z = pcall(function() return value.x, value.y, value.z end)
    return ok and isFiniteNumber(x) and isFiniteNumber(y) and isFiniteNumber(z)
end

local function componentWorldPosition(component)
    local okMatrix, matrix = pcall(function() return component:GetLocalToWorld() end)
    if not okMatrix or matrix == nil then
        return nil
    end
    local okPosition, position = pcall(function() return Matrix.GetTranslation(matrix) end)
    if not okPosition or not validVector(position) then
        okPosition, position = pcall(function()
            return GetSingleton("Matrix"):GetTranslation(matrix)
        end)
    end
    return okPosition and validVector(position) and position or nil
end

local function activeCameraPosition(cameraSystem, fallbackComponent)
    local position = cameraDiagnosticCall(cameraSystem, "GetActiveCameraWorldPosition") or
        cameraDiagnosticCall(cameraSystem, "GetActiveCameraPosition")
    if validVector(position) then
        return position
    end
    local okTransform, transform = pcall(function()
        return cameraSystem:GetActiveCameraWorldTransform()
    end)
    if okTransform and transform ~= nil then
        local okPosition, transformPosition = pcall(function()
            return transform.position or transform:GetPosition()
        end)
        if okPosition and validVector(transformPosition) then
            return transformPosition
        end
    end
    return componentWorldPosition(fallbackComponent)
end

local function horizontalYaw(forward)
    local x, y = forward.x, forward.y
    local length = math.sqrt(x * x + y * y)
    if length < 0.000001 then
        return nil
    end
    if x > 0 then
        return math.deg(math.atan(y / x))
    elseif x < 0 then
        return math.deg(math.atan(y / x) + (y >= 0 and math.pi or -math.pi))
    end
    return y >= 0 and 90.0 or -90.0
end

local function angularDifference(left, right)
    return (left - right + 180.0) % 360.0 - 180.0
end

local function activeCameraDiagnostics(cameraSystem)
    local position = cameraDiagnosticCall(cameraSystem, "GetActiveCameraWorldPosition")
    if position == nil then
        position = cameraDiagnosticCall(cameraSystem, "GetActiveCameraPosition")
    end
    local projection = cameraDiagnosticCall(cameraSystem, "GetActiveCameraProjectionMatrix")
    local view = cameraDiagnosticCall(cameraSystem, "GetActiveCameraViewMatrix")
    local viewport = cameraDiagnosticCall(cameraSystem, "GetActiveCameraViewport")
    local width, height = nil, nil
    if type(viewport) == "table" then
        width, height = viewport.width or viewport.x, viewport.height or viewport.y
    end
    local activeViewport = isFiniteNumber(width) and isFiniteNumber(height)
    if not activeViewport then
        width, height = GetDisplayResolution()
    end
    return {
        position = validVector(position) and position or nil,
        projection = numberArrayJson(projection, 16),
        view = numberArrayJson(view, 16),
        viewportWidth = isFiniteNumber(width) and width or nil,
        viewportHeight = isFiniteNumber(height) and height or nil,
        viewportSource = activeViewport and "active_camera" or "display_resolution",
    }
end

local function writeSessionMetadata(session, state)
    local temporary = session.metadataPath .. ".tmp"
    local output, openError = io.open(temporary, "wb")
    if output == nil then
        log("Metadata warning: cannot open " .. session.metadataPath .. ": " .. tostring(openError))
        return false
    end

    local function writeChunk(...)
        local result, writeError = output:write(...)
        if result == nil then
            log("Metadata warning: cannot write " .. session.metadataPath .. ": " .. tostring(writeError))
            return false
        end
        return true
    end

    if not writeChunk("{\n", string.format("  \"schema_version\":1,\n  \"session_id\":\"%s\",\n",
        jsonEscape(session.sessionId))) or
        not writeChunk(string.format("  \"location\":{\"position\":%s,\"yaw_deg\":%.9f},\n",
            vectorJson(session.location.position), session.location.yaw)) or
        not writeChunk(string.format("  \"horizontal_fov_deg\":%.9f,\n  \"vertical_fov_deg\":%.9f,\n",
            session.horizontalFov, session.verticalFov)) or
        not writeChunk(string.format("  \"state\":\"%s\",\n  \"poses\":[\n", jsonEscape(state))) then
        output:close()
        os.remove(temporary)
        return false
    end
    for index, record in ipairs(session.metadataRecords) do
        if not writeChunk(string.format(
            "    {\"index\":%d,\"row\":%d,\"column\":%d,\"commanded_yaw_deg\":%.9f," ..
            "\"commanded_pitch_deg\":%.9f,\"observed_pitch_deg\":%.9f,\"forward\":%s," ..
            "\"right\":%s,\"up\":%s,\"settle_seconds\":%.6f,\"screenshot_path\":\"%s\"%s%s%s%s%s%s}%s\n",
            record.index, record.row, record.column, record.commandedYaw, record.commandedPitch,
            record.observedPitch, vectorJson(record.forward), vectorJson(record.right),
            vectorJson(record.up), record.settleSeconds, jsonEscape(record.screenshotPath),
            record.cameraPosition ~= nil and string.format(",\"camera_position\":%s", vectorJson(record.cameraPosition)) or "",
            record.cameraDisplacement ~= nil and string.format(",\"camera_displacement\":%.9f", record.cameraDisplacement) or "",
            record.horizontalFov ~= nil and string.format(",\"horizontal_fov_deg\":%.9f,\"vertical_fov_deg\":%.9f", record.horizontalFov, record.verticalFov) or "",
            record.viewportWidth ~= nil and string.format(",\"viewport\":{\"width\":%d,\"height\":%d,\"source\":\"%s\"}", record.viewportWidth, record.viewportHeight, record.viewportSource) or "",
            record.projectionMatrix ~= nil and string.format(",\"projection_matrix_row_major\":%s", record.projectionMatrix) or "",
            record.viewMatrix ~= nil and string.format(",\"view_matrix_row_major\":%s", record.viewMatrix) or "",
            index < #session.metadataRecords and "," or "")) then
            output:close()
            os.remove(temporary)
            return false
        end
    end
    if not writeChunk("  ]\n}\n") then
        output:close()
        os.remove(temporary)
        return false
    end
    local closed, closeError = output:close()
    if not closed then
        os.remove(temporary)
        log("Metadata warning: cannot close " .. session.metadataPath .. ": " .. tostring(closeError))
        return false
    end
    local verificationInput, verificationOpenError = io.open(temporary, "rb")
    if verificationInput == nil then
        log("Metadata warning: cannot verify " .. temporary .. ": " ..
            tostring(verificationOpenError))
        return false
    end
    local content = verificationInput:read("*a")
    verificationInput:close()
    local validJson, decoded = pcall(json.decode, content)
    if not validJson or type(decoded) ~= "table" then
        log("Metadata warning: generated JSON is invalid; retaining " .. temporary ..
            " and the previous session metadata: " .. tostring(decoded))
        return false
    end
    os.remove(session.metadataPath)
    local renamed = os.rename(temporary, session.metadataPath)
    if not renamed then
        os.remove(temporary)
        log("Metadata warning: cannot publish " .. session.metadataPath)
        return false
    end
    return true
end

local function discardSessionMetadata(session)
    os.remove(session.metadataPath)
    os.remove(session.metadataPath .. ".tmp")
end

local function finalizeAbortedSessionMetadata(session)
    if not writeSessionMetadata(session, "aborted") then
        log("Metadata warning: aborted session could not be published; retaining existing files.")
    end
end

local function discardBridgeFiles()
    os.remove(bridgeFile("request"))
    os.remove(bridgeFile("ack"))
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
    local horizontal = captureConfig.captureFov
    if horizontal ~= nil then
        vertical = math.deg(2.0 * math.atan(math.tan(math.rad(horizontal) / 2.0) / aspect))
    else
        horizontal = math.deg(2.0 * math.atan(math.tan(math.rad(vertical) / 2.0) * aspect))
    end
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
    -- A rectilinear frame's longitude coverage changes across its vertical extent.
    -- Keeping the yaw count uniform avoids gaps between latitude rows near the poles.
    local columns = math.max(1, math.ceil(360.0 / guardedYawStep))
    for row = 0, rows - 1 do
        local pitch = rows == 1 and 0.0 or -maxPitch + (2.0 * maxPitch * row / (rows - 1))
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
        columns = columns,
        rows = rows,
        poses = plan,
    }, nil
end

local function captureFovPreview(displayFov)
    local cameraSystem = Game.GetCameraSystem()
    local aspect = nil
    if cameraSystem ~= nil then
        local okAspect, value = pcall(function() return cameraSystem:GetAspectRatio() end)
        if okAspect and isFiniteNumber(value) and value > 0 then
            aspect = value
        end
    end
    local screenshotWidth, screenshotHeight = GetDisplayResolution()
    if aspect == nil and isFiniteNumber(screenshotWidth) and isFiniteNumber(screenshotHeight) and screenshotHeight > 0 then
        aspect = screenshotWidth / screenshotHeight
    end
    if aspect == nil or not isFiniteNumber(displayFov) or displayFov <= 0 or displayFov >= 180 then
        return nil, "camera aspect unavailable"
    end
    local vertical = math.deg(2.0 * math.atan(math.tan(math.rad(displayFov) / 2.0) / aspect))
    local plan = buildFullSpherePlan(displayFov, vertical, captureConfig.overlap, captureConfig.adaptiveYawGuard)
    if plan == nil then
        return nil, "capture plan unavailable"
    end
    if not isFiniteNumber(screenshotWidth) or not isFiniteNumber(screenshotHeight) or
        screenshotWidth <= 0 or screenshotHeight <= 0 then
        return { shots = #plan.poses }, nil
    end
    local focalX = screenshotWidth / (2.0 * math.tan(math.rad(displayFov) / 2.0))
    local panoramaWidth = math.max(2, math.floor(2.0 * math.pi * focalX + 0.5))
    local panoramaHeight = math.max(1, math.floor(panoramaWidth / 2.0 + 0.5))
    return {
        shots = #plan.poses,
        megapixels = panoramaWidth * panoramaHeight / 1000000.0,
        screenshotWidth = screenshotWidth,
        screenshotHeight = screenshotHeight,
    }, nil
end

local function estimateCaptureSeconds(shotCount)
    if bridgeCaptureRange == "unknown" then
        return nil
    end
    local screenshotSeconds = bridgeCaptureRange == "hdr" and 1.0 or 0.1
    return captureConfig.settleSeconds + shotCount * screenshotSeconds +
        math.max(0, shotCount - 1) * math.max(captureConfig.settleSeconds,
            captureConfig.screenshotCooldownSeconds)
end

local function formatDuration(seconds)
    local rounded = math.max(0, math.floor(seconds + 0.5))
    return string.format("~%dm %02ds", math.floor(rounded / 60), rounded % 60)
end

local function previewCaptureFov()
    if isFiniteNumber(captureConfig.captureFov) then
        return captureConfig.captureFov
    end
    return nativeSettingsCaptureFovDefault()
end

updateCaptureSummary = function()
    local option = nativeSettingsCaptureSummaryOption
    if option == nil then
        return
    end
    local displayFov = previewCaptureFov()
    local preview, previewError = captureFovPreview(displayFov)
    if preview == nil then
        option.text = "Capture estimate unavailable: " .. tostring(previewError)
    elseif preview.megapixels == nil then
        option.text = string.format("Capture estimate: %d shots", preview.shots)
    else
        local duration = estimateCaptureSeconds(preview.shots)
        local timing = duration and formatDuration(duration) or "timing unavailable"
        option.text = string.format("Capture estimate: %d shots / %s · ~%.1f MP",
            preview.shots, timing, preview.megapixels)
    end
    if option.textWidget ~= nil then
        pcall(function() option.textWidget:SetText(option.text) end)
    end
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
        return false, "player changed during capture"
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
            log("Production session: conflicting time dilation is active for " .. reason)
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

    if environment.inputRestrictions == nil then
        local inputLocked, inputError = applyInputRestrictions(environment)
        if not inputLocked then
            return false, inputError
        end
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

    local ammCameraActive = false
    pcall(function()
        ammCameraActive = AMM ~= nil and AMM.Director ~= nil and
            AMM.Director.activeCamera ~= nil
    end)
    if ammCameraActive then
        return "Appearance Menu Mod camera is active; deactivate it before starting a panorama capture"
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
    local diagnostics = activeCameraDiagnostics(cameraSystem)
    return {
        forward = forward,
        right = right,
        up = up,
        horizontalFov = horizontal,
        verticalFov = fov,
        pitch = math.deg(math.asin(clampedForwardZ)),
        basisValid = basisValid,
        cameraPosition = diagnostics.position,
        projectionMatrix = diagnostics.projection,
        viewMatrix = diagnostics.view,
        viewportWidth = diagnostics.viewportWidth,
        viewportHeight = diagnostics.viewportHeight,
        viewportSource = diagnostics.viewportSource,
    }, nil
end

local function logPoseMetadata(session, pose, observed)
    local f = observed.forward
    local r = observed.right
    local u = observed.up
    devLog(string.format(
        "POSE_METADATA index=%d/%d row=%d column=%d commanded_yaw=%.6f commanded_pitch=%.6f " ..
        "observed_forward=(%.9f,%.9f,%.9f) observed_right=(%.9f,%.9f,%.9f) " ..
        "observed_up=(%.9f,%.9f,%.9f) hfov=%.9f vfov=%.9f settle_seconds=%.6f " ..
        "basis_valid=%s observed_pitch=%.6f",
        session.index, #session.plan.poses, pose.row, pose.column, pose.yaw, pose.pitch,
        f.x, f.y, f.z, r.x, r.y, r.z, u.x, u.y, u.z, observed.horizontalFov,
        observed.verticalFov, session.lastSettleSeconds, tostring(observed.basisValid), observed.pitch))
    local cameraDisplacement = nil
    if observed.cameraPosition ~= nil and session.referenceCameraPosition ~= nil then
        local dx = observed.cameraPosition.x - session.referenceCameraPosition.x
        local dy = observed.cameraPosition.y - session.referenceCameraPosition.y
        local dz = observed.cameraPosition.z - session.referenceCameraPosition.z
        cameraDisplacement = math.sqrt(dx * dx + dy * dy + dz * dz)
    elseif observed.cameraPosition ~= nil then
        session.referenceCameraPosition = observed.cameraPosition
    end
    if cameraDisplacement ~= nil then
        devLog(string.format("POSE_CAMERA_DIAGNOSTIC index=%d displacement=%.9f", session.index,
            cameraDisplacement))
    elseif observed.cameraPosition == nil then
        devLog(string.format("POSE_CAMERA_DIAGNOSTIC index=%d active camera position unavailable",
            session.index))
    end
    if observed.projectionMatrix == nil or observed.viewMatrix == nil then
        devLog(string.format(
            "POSE_CAMERA_DIAGNOSTIC index=%d projection/view matrix unavailable", session.index))
    end
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
        cameraPosition = observed.cameraPosition,
        cameraDisplacement = cameraDisplacement,
        horizontalFov = observed.horizontalFov,
        verticalFov = observed.verticalFov,
        viewportWidth = observed.viewportWidth,
        viewportHeight = observed.viewportHeight,
        viewportSource = observed.viewportSource,
        projectionMatrix = observed.projectionMatrix,
        viewMatrix = observed.viewMatrix,
    }
end

destroyStandaloneCamera = function(environment)
    local standalone = environment.standaloneCamera
    if standalone == nil then
        return
    end
    if standalone.component ~= nil then
        pcall(function() standalone.component:Deactivate(0, false) end)
    end
    if environment.playerCamera ~= nil then
        pcall(function() environment.playerCamera:Activate() end)
    end
    if standalone.handle ~= nil then
        pcall(function() standalone.handle:Dispose() end)
    end
    environment.standaloneCamera = nil
end

local function applyStandalonePose(environment, yawDegrees)
    local angles = EulerAngles.new(0, environment.targetPitch, yawDegrees)
    environment.standaloneCamera.component:SetLocalOrientation(angles:ToQuat())
    environment.waitFrames = 2
end

local function beginProductionCapture(environment)
    local captureHorizontal, captureVertical, captureFovError = effectiveFov()
    if captureHorizontal == nil then
        return false, "capture FoV readback failed: " .. tostring(captureFovError)
    end
    local plan, planError = buildFullSpherePlan(
        captureHorizontal, captureVertical, captureConfig.overlap, captureConfig.adaptiveYawGuard)
    if plan == nil then
        return false, planError
    end
    local firstPose = plan.poses[1]
    environment.targetYaw = firstPose.yaw
    environment.targetPitch = firstPose.pitch
    bridgeSessionCounter = bridgeSessionCounter + 1
    local sessionId = string.format("%d-%d", os.time(), bridgeSessionCounter)
    productionSession = {
        plan = plan,
        index = 1,
        sessionId = sessionId,
        horizontalFov = captureHorizontal,
        verticalFov = captureVertical,
        settleSeconds = captureConfig.settleSeconds,
        screenshotCooldownSeconds = captureConfig.screenshotCooldownSeconds,
        screenshotAckTimeoutSeconds = captureConfig.screenshotAckTimeoutSeconds,
        lastSettleSeconds = 0.0,
        awaitingScreenshot = false,
        screenshotWaitElapsed = 0.0,
        screenshotCooldownRemaining = 0.0,
        metadataPath = bridgeFile("pano-" .. sessionId .. ".json"),
        location = {
            position = environment.position,
            yaw = environment.originalYaw,
        },
        metadataRecords = {},
        pendingMetadata = nil,
        metadataFailed = false,
    }
    if not writeSessionMetadata(productionSession, "active") then
        discardSessionMetadata(productionSession)
        productionSession = nil
        return false, "initial metadata publication failed"
    end
    if exEntitySpawner == nil or type(exEntitySpawner.Spawn) ~= "function" then
        discardSessionMetadata(productionSession)
        productionSession = nil
        return false, "Codeware exEntitySpawner is unavailable"
    end
    local cameraPosition = environment.initialCameraPosition
    if cameraPosition == nil then
        discardSessionMetadata(productionSession)
        productionSession = nil
        return false, "active gameplay-camera optical center is unavailable"
    end
    local transform = environment.player:GetWorldTransform()
    transform:SetPosition(cameraPosition)
    local spawnOk, entityID = pcall(function()
        return exEntitySpawner.Spawn(STANDALONE_CAMERA_PATH, transform, "")
    end)
    if not spawnOk or entityID == nil then
        discardSessionMetadata(productionSession)
        productionSession = nil
        return false, "standalone camera spawn failed"
    end
    environment.standaloneCamera = {
        entityID = entityID,
        handle = nil,
        component = nil,
        referencePosition = nil,
        spawnElapsed = 0.0,
    }
    environment.state = "camera_spawn_pending"
    log(string.format("Production session started: %d poses, %dx%d, HFoV=%.3f VFoV=%.3f, adaptive yaw guard=%.1f%%.",
        #plan.poses, plan.columns, plan.rows, captureHorizontal, captureVertical,
        plan.adaptiveYawGuard * 100.0))
    return true, nil
end

local function startProductionSession()
    if productionSession ~= nil or environmentSequence ~= nil then
        devLog("Production session already active.")
        return
    end
    syncNativeSettings()
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
        log("Production session requires normal gameplay view; exit Photo Mode first.")
        return
    end
    local player = Game.GetPlayer()
    local cameraSystem = Game.GetCameraSystem()
    local playerCamera = player and player:GetFPPCameraComponent()
    local playerHash = player and entityHash(player)
    if player == nil or playerHash == nil or cameraSystem == nil or playerCamera == nil then
        log("Production session requires an active player and gameplay camera.")
        return
    end
    local initialCameraPosition = activeCameraPosition(cameraSystem, playerCamera)
    local okForward, initialForward = pcall(function()
        return cameraSystem:GetActiveCameraForward()
    end)
    local initialCameraYaw = okForward and initialForward and horizontalYaw(initialForward) or nil
    local initialCameraPitch = okForward and initialForward and
        math.deg(math.asin(math.max(-1.0, math.min(1.0, initialForward.z)))) or nil
    if initialCameraPosition == nil or initialCameraYaw == nil or initialCameraPitch == nil then
        log("Production session cancelled: active camera origin or heading is unavailable.")
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
    environmentSequence = {
        playerCamera = playerCamera,
        initialCameraPosition = initialCameraPosition,
        initialCameraYaw = initialCameraYaw,
        initialCameraPitch = initialCameraPitch,
        player = player,
        playerHash = playerHash,
        position = Vector4.new(player:GetWorldPosition().x, player:GetWorldPosition().y,
            player:GetWorldPosition().z, player:GetWorldPosition().w),
        originalYaw = player:GetWorldYaw(),
        hud = hud,
        meshes = meshes,
        state = "starting",
        waitFrames = 0,
        settleElapsed = 0.0,
        settleSecondsRequired = captureConfig.settleSeconds,
        targetYaw = 0.0,
        targetPitch = 0.0,
        timeApplied = false,
        restoreRequested = false,
    }
    local started, startError = beginProductionCapture(environmentSequence)
    if not started then
        restoreEnvironmentControls(environmentSequence)
        environmentSequence = nil
        log("Production session cancelled: " .. tostring(startError))
    end
end

local function queueNextProductionPose()
    if productionSession == nil or environmentSequence == nil then
        return
    end
    if productionSession.index >= #productionSession.plan.poses then
        environmentSequence.restoreRequested = true
        devLog("Production session: final screenshot acknowledged; restoration queued.")
        return
    end
    productionSession.index = productionSession.index + 1
    local pose = productionSession.plan.poses[productionSession.index]
    environmentSequence.targetYaw = pose.yaw
    environmentSequence.targetPitch = pose.pitch
    environmentSequence.settleElapsed = 0.0
    environmentSequence.state = "rotated_pending"
    applyStandalonePose(environmentSequence, pose.yaw)
    devLog(string.format("Production pose %d/%d queued: row=%d column=%d yaw=%.3f pitch=%.3f.",
        productionSession.index, #productionSession.plan.poses,
        pose.row, pose.column, pose.yaw, pose.pitch))
end

registerInput("panorama_capture_start", "Panorama: start full-sphere pose session", function(down)
    if down then
        startProductionSession()
    end
end)

registerDevelopmentInput("panorama_capture_advance", "Panorama: advance full-sphere pose", function(down)
    if not down then
        return
    end
    if productionSession == nil or environmentSequence == nil then
        devLog("No production session is active.")
        return
    end
    if captureConfig.automatedScreenshots then
        devLog("Automated screenshot mode is active; ReShade acknowledgement advances the pose.")
        return
    end
    if environmentSequence.state ~= "active" then
        devLog("Production pose is not settled; wait for the ready log.")
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
    syncNativeSettings()
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
        fovStatus, captureConfig.overlap * 100.0,
        productionSession and productionSession.settleSeconds or captureConfig.settleSeconds))
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

registerDevelopmentInput("panorama_capture_status", "Panorama: report capture status", function(down)
    if down then
        reportStatus()
    end
end)

local function abortEnvironment(reason)
    if environmentSequence == nil then
        return
    end
    discardBridgeFiles()
    if productionSession ~= nil then
        finalizeAbortedSessionMetadata(productionSession)
    end
    destroyStandaloneCamera(environmentSequence)
    restoreEnvironmentControls(environmentSequence)
    environmentSequence = nil
    productionSession = nil
    log("Production session aborted (" .. reason .. ").")
end

local function requestProductionScreenshot()
    if productionSession == nil or environmentSequence == nil then
        return
    end
    local observed, observedError = observedCameraMetadata()
    if observed == nil then
        log("Production session cancelled: pre-screenshot camera readback failed: " .. tostring(observedError))
        environmentSequence.restoreRequested = true
        return
    end
    if environmentSequence.standaloneCamera ~= nil and
        environmentSequence.standaloneCamera.handle ~= nil then
        local positionOk, position = pcall(function()
            return environmentSequence.standaloneCamera.handle:GetWorldPosition()
        end)
        if positionOk and validVector(position) then
            observed.cameraPosition = position
        end
    end
    if observed.cameraPosition == nil then
        log("Production session cancelled: standalone camera position readback failed")
        environmentSequence.restoreRequested = true
        return
    end
    if math.abs(observed.horizontalFov - productionSession.horizontalFov) > CAMERA_FOV_TOLERANCE_DEGREES or
        math.abs(observed.verticalFov - productionSession.verticalFov) > CAMERA_FOV_TOLERANCE_DEGREES then
        log(string.format(
            "Production session cancelled: standalone camera FoV mismatch (expected %.3fx%.3f, observed %.3fx%.3f)",
            productionSession.horizontalFov, productionSession.verticalFov,
            observed.horizontalFov, observed.verticalFov))
        environmentSequence.restoreRequested = true
        return
    end
    local reference = environmentSequence.standaloneCamera.referencePosition
    local dx = observed.cameraPosition.x - reference.x
    local dy = observed.cameraPosition.y - reference.y
    local dz = observed.cameraPosition.z - reference.z
    if math.sqrt(dx * dx + dy * dy + dz * dz) > CAMERA_POSITION_TOLERANCE then
        log("Production session cancelled: standalone camera moved beyond position tolerance")
        environmentSequence.restoreRequested = true
        return
    end
    productionSession.pendingMetadata = logPoseMetadata(
        productionSession, productionSession.plan.poses[productionSession.index], observed)
    local token = string.format("pano-%s-%03d", productionSession.sessionId, productionSession.index)
    local requestOk, requestError = writeBridgeRequest(
        productionSession.sessionId, productionSession.index, token)
    if not requestOk then
        log("Production session cancelled: " .. tostring(requestError))
        environmentSequence.restoreRequested = true
        return
    end
    productionSession.awaitingScreenshot = true
    productionSession.screenshotWaitElapsed = 0.0
    environmentSequence.state = "awaiting_screenshot"
    devLog(string.format("Production pose ready: %d/%d; ReShade screenshot requested.",
        productionSession.index, #productionSession.plan.poses))
end

registerForEvent("onUpdate", function(deltaTime)
    if type(deltaTime) == "number" and deltaTime > 0 then
        bridgeStatusPollElapsed = bridgeStatusPollElapsed + deltaTime
        if bridgeStatusPollElapsed >= 1.0 then
            bridgeStatusPollElapsed = 0.0
            refreshBridgeCaptureRange()
            if updateCaptureSummary ~= nil then
                updateCaptureSummary()
            end
        end
    end
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
        if environmentSequence.state == "camera_spawn_pending" then
            local standalone = environmentSequence.standaloneCamera
            if standalone == nil then
                abortEnvironment("standalone camera state is missing")
                return
            end
            if type(deltaTime) == "number" and deltaTime > 0 then
                standalone.spawnElapsed = standalone.spawnElapsed + deltaTime
            end
            local handle = Game.FindEntityByID(standalone.entityID)
            if handle ~= nil then
                local componentOk, component = pcall(function()
                    return handle:FindComponentByName("camera")
                end)
                if not componentOk then
                    abortEnvironment("standalone camera component lookup failed")
                    return
                end
                if component == nil then
                    abortEnvironment("standalone camera component is unavailable")
                    return
                end
                standalone.handle = handle
                standalone.component = component
                local positionOk, position = pcall(function() return handle:GetWorldPosition() end)
                if not positionOk or not validVector(position) then
                    abortEnvironment("standalone camera position readback failed")
                    return
                end
                standalone.referencePosition = position
                local fovOk = pcall(function() component:SetFOV(productionSession.verticalFov) end)
                if not fovOk then
                    abortEnvironment("standalone camera FoV assignment failed")
                    return
                end
                environmentSequence.targetPitch = environmentSequence.initialCameraPitch
                local activationOk = pcall(function()
                    applyStandalonePose(environmentSequence, 0.0)
                    component:Activate(0, false)
                end)
                if not activationOk then
                    abortEnvironment("standalone camera activation failed")
                    return
                end
                local controlsOk, controlsError = applyEnvironmentControls(environmentSequence)
                if not controlsOk then
                    abortEnvironment(controlsError)
                    return
                end
                environmentSequence.state = "initial_exposure_hold"
                environmentSequence.settleElapsed = 0.0
                return
            end
            if standalone.spawnElapsed >= CAMERA_SPAWN_TIMEOUT_SECONDS then
                abortEnvironment("standalone camera spawn timed out")
            end
            return
        end
        if environmentSequence.state == "initial_exposure_hold" then
            if type(deltaTime) == "number" and deltaTime > 0 then
                environmentSequence.settleElapsed = environmentSequence.settleElapsed + deltaTime
            end
            if environmentSequence.settleElapsed >= INITIAL_EXPOSURE_HOLD_SECONDS then
                local firstPose = productionSession.plan.poses[1]
                environmentSequence.targetYaw = firstPose.yaw
                environmentSequence.targetPitch = firstPose.pitch
                environmentSequence.settleElapsed = 0.0
                environmentSequence.state = "rotated_pending"
                applyStandalonePose(environmentSequence, firstPose.yaw)
            end
            return
        end
        if productionSession ~= nil and not productionSession.awaitingScreenshot and
            productionSession.screenshotCooldownRemaining > 0 and type(deltaTime) == "number" and
            deltaTime > 0 then
            productionSession.screenshotCooldownRemaining = math.max(0.0,
                productionSession.screenshotCooldownRemaining - deltaTime)
        end
        if productionSession ~= nil and productionSession.awaitingScreenshot then
            if type(deltaTime) == "number" and deltaTime > 0 then
                productionSession.screenshotWaitElapsed =
                    productionSession.screenshotWaitElapsed + deltaTime
            end
            if productionSession.screenshotWaitElapsed >= productionSession.screenshotAckTimeoutSeconds then
                productionSession.awaitingScreenshot = false
                discardBridgeFiles()
                log("Production session cancelled: ReShade screenshot acknowledgement timed out.")
                environmentSequence.restoreRequested = true
            else
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
                        discardBridgeFiles()
                        environmentSequence.restoreRequested = true
                    else
                        local metadata = productionSession.pendingMetadata
                        if metadata ~= nil then
                            metadata.screenshotPath = acknowledgement.path
                            productionSession.metadataRecords[#productionSession.metadataRecords + 1] = metadata
                            productionSession.pendingMetadata = nil
                        end
                        local completed = productionSession.index >= #productionSession.plan.poses
                        local metadataState = completed and "completed" or "active"
                        if not writeSessionMetadata(productionSession, metadataState) then
                            productionSession.metadataFailed = true
                            log("Production session cancelled: metadata publication failed.")
                            environmentSequence.restoreRequested = true
                        else
                            devLog(string.format("Production screenshot acknowledged: pose %d/%d path=%s",
                                productionSession.index, #productionSession.plan.poses, acknowledgement.path))
                            if completed then
                                queueNextProductionPose()
                            else
                                productionSession.screenshotCooldownRemaining =
                                    productionSession.screenshotCooldownSeconds
                                queueNextProductionPose()
                            end
                        end
                    end
                else
                    devLog("Production screenshot acknowledgement ignored: request identity mismatch.")
                end
                end
            end
        end
        if environmentSequence.restoreRequested and environmentSequence.state ~= "restore_pending" and
            environmentSequence.state ~= "restore_correcting" then
            environmentSequence.state = "restore_pending"
            environmentSequence.waitFrames = 2
        elseif environmentSequence.waitFrames > 0 then
            environmentSequence.waitFrames = environmentSequence.waitFrames - 1
        elseif environmentSequence.state == "rotated_pending" then
            local hidden, hideError = hideHud(environmentSequence)
            local meshesHidden, meshError = hideCaptureMeshes(environmentSequence)
            if not hidden or not meshesHidden then
                log("Production session cancelled: transition re-hide failed: " ..
                    tostring(hideError or meshError))
                environmentSequence.restoreRequested = true
                environmentSequence.state = "active"
            else
                environmentSequence.state = "settling"
                environmentSequence.settleElapsed = 0.0
                environmentSequence.settleSecondsRequired = productionSession.settleSeconds
            end
        elseif environmentSequence.state == "settling" then
            if type(deltaTime) == "number" and deltaTime > 0 then
                environmentSequence.settleElapsed = environmentSequence.settleElapsed + deltaTime
            end
            if environmentSequence.settleElapsed >= environmentSequence.settleSecondsRequired then
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
                    local observedYaw = horizontalYaw(observed.forward)
                    local expectedYaw = environmentSequence.initialCameraYaw + pose.yaw
                    local yawError = observedYaw and angularDifference(expectedYaw, observedYaw) or nil
                    if math.abs(pitchError) > CAMERA_PITCH_TOLERANCE_DEGREES then
                        log(string.format(
                            "Production session cancelled: pose %d pitch error %.3f exceeds %.3f tolerance.",
                            productionSession.index, pitchError, CAMERA_PITCH_TOLERANCE_DEGREES))
                        environmentSequence.restoreRequested = true
                        environmentSequence.state = "active"
                    elseif yawError == nil or math.abs(yawError) > CAMERA_YAW_TOLERANCE_DEGREES then
                        log(string.format(
                            "Production session cancelled: pose %d yaw error %s exceeds %.3f tolerance.",
                            productionSession.index,
                            yawError and string.format("%.3f", yawError) or "unavailable",
                            CAMERA_YAW_TOLERANCE_DEGREES))
                        environmentSequence.restoreRequested = true
                        environmentSequence.state = "active"
                    else
                        productionSession.pendingMetadata = logPoseMetadata(productionSession, pose, observed)
                        if captureConfig.automatedScreenshots then
                            if productionSession.screenshotCooldownRemaining > 0 then
                                environmentSequence.state = "awaiting_screenshot_cooldown"
                            else
                                requestProductionScreenshot()
                            end
                        else
                            devLog(string.format(
                                "Production pose ready: %d/%d; capture screenshot, then advance.",
                                productionSession.index, #productionSession.plan.poses))
                            environmentSequence.state = "active"
                        end
                    end
                end
            end
        elseif environmentSequence.state == "awaiting_screenshot_cooldown" then
            if productionSession.screenshotCooldownRemaining <= 0 then
                requestProductionScreenshot()
            end
        elseif environmentSequence.state == "restore_pending" then
            destroyStandaloneCamera(environmentSequence)
            environmentSequence.waitFrames = 2
            environmentSequence.state = "restore_correcting"
        elseif environmentSequence.state == "restore_correcting" then
            local completedProductionSession = false
            if productionSession ~= nil then
                local completed = not productionSession.metadataFailed and
                    productionSession.index >= #productionSession.plan.poses and
                    productionSession.pendingMetadata == nil
                completedProductionSession = completed
                if completed then
                    if not writeSessionMetadata(productionSession, "completed") then
                        productionSession.metadataFailed = true
                        completedProductionSession = false
                        log("Production session aborted: final metadata publication failed.")
                        finalizeAbortedSessionMetadata(productionSession)
                    end
                else
                    finalizeAbortedSessionMetadata(productionSession)
                    discardBridgeFiles()
                end
            end
            restoreEnvironmentControls(environmentSequence)
            environmentSequence = nil
            productionSession = nil
            if completedProductionSession then
                log("Production session completed.")
            else
                log("Production session aborted.")
            end
        end
    end

end)

registerForEvent("onInit", function()
    loadPersistentSettings()
    refreshBridgeCaptureRange()
    registerNativeSettings()
    if updateCaptureSummary ~= nil then
        updateCaptureSummary()
    end
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
        abortEnvironment("CET shutdown/reload")
    end
end)
