#pragma once

#include <algorithm>
#include <optional>
#include <string_view>

namespace UsmapGen {

//! UnrealNames.inl: the hardcoded names of every version from 4.18 to 5.9, which FName::StaticInit enters first.
inline constexpr std::string_view hardcodedNames[] = {
    "Actor", "AdvanceFrame", "AI", "All", "Aniso", "ArrayProperty", "AudioThread", "Available",
    "BeaconNetDriver", "BeaconPort", "BoolProperty", "Box", "BoxSphereBounds", "BSP", "ByteProperty", "Camera",
    "Class", "Cmd", "Color", "Components", "Core", "CoreUObject", "Cylinder", "Default",
    "DelegateProperty", "DemoNetDriver", "DeviceID", "DGram", "Double", "DoubleProperty", "Editor", "EditorKeyBindings",
    "EditorLayout", "EditorSettings", "Engine", "Enum", "EnumProperty", "Error", "ExecuteUbergraph", "Exit",
    "ExpressionInput", "FALSE", "FloatProperty", "FlushNetDormancy", "FontCharacter", "Function", "Game", "GameNetDriver",
    "GamePort", "GameSession", "GameThread", "Global", "Groups", "High", "ID", "ImpactVel",
    "Inactive", "Info", "Init", "InitChild2StartBone", "InitialState", "Input", "Int16Property", "Int64Property",
    "Int8Property", "Interface", "InterfaceProperty", "InterpCurveFloat", "InterpCurveQuat", "InterpCurveTwoVectors", "InterpCurveVector", "InterpCurveVector2D",
    "IntPoint", "IntProperty", "IntRect", "LazyObjectProperty", "LightMapResolution", "Linear", "LinearColor", "Location",
    "Low", "Map", "MapProperty", "Matrix", "MeshEmitterDynamicParameter", "MeshEmitterVertexColor", "MoveActor", "MulticastDelegateProperty",
    "Name", "NameProperty", "NavMesh", "NetworkGUID", "None", "Object", "ObjectArchetype", "ObjectProperty",
    "ObjectRedirector", "OtherChildren", "Outer", "Package", "PackageMetaData", "PartySession", "PendingNetDriver", "PerformanceCapture",
    "PerfWarning", "PersistentLevel", "Plane", "Playing", "Point", "Pointer", "Quat", "RawDistributionFloat",
    "RawDistributionVector", "RemoteRole", "RenderThread", "Role", "RootStat", "Rotation", "Rotator", "RotatorProperty",
    "ScriptStruct", "SelectionColor", "Self", "SetProperty", "SHVector", "Skip", "SlideVel", "SoftObjectProperty",
    "SoundCue", "SoundCueLocalized", "Spectating", "Sphere", "State", "Stream", "StrProperty", "StructProperty",
    "Super", "Team", "TextProperty", "TextureOffset1Parameter", "TextureOffsetParameter", "TextureScaleParameter", "TheWorld", "Timer",
    "Transform", "TRUE", "UI", "UInt16Property", "UInt32Property", "UInt64Property", "UnGrouped", "Untitled",
    "Vector", "Vector2D", "Vector4", "VectorProperty", "VoiceChat", "Warning"
};

//! A game build ignores the case of names, so a bare enumerator equal to a hardcoded name takes its spelling.
inline std::string_view runtimeEnumName(std::string_view full) {
    const auto equal = [&](std::string_view name) {
        return std::ranges::equal(name, full, [](char a, char b) {
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
            return lower(a) == lower(b);
        });
    };
    const auto* found = std::ranges::find_if(hardcodedNames, equal);
    return found == std::ranges::end(hardcodedNames) ? full : *found;
}

}
