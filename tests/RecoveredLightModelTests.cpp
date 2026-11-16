// Standalone: cl /nologo /std:c++20 /W4 /WX /EHsc /O2 tests\RecoveredLightModelTests.cpp
#include "../Reloaded.Editor/RecoveredLightModel.h"
#include "../Reloaded.Editor/RecoveredLightTwin.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
using namespace RecoveredLights;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}

void Terms()
{
    Light light;
    light.location = {0, 0, 100};
    light.lightRadius = 7;  // reach 200
    Check(light.Radius() == 200, "reach is 25 (LightRadius + 1)");
    // Straight below, half way out: S(0.5) = 0.5, cosine 1.
    Check(std::fabs(light.Surface({0, 0, 0}, {0, 0, 1}, false) - 0.5) < 1e-12, "point term S(x) cos");
    Check(light.Surface({0, 0, 0}, {0, 0, -1}, false) == 0, "a surface facing away takes nothing");
    Check(std::fabs(light.Surface({0, 0, 0}, {0, 0, -1}, true) - 0.5) < 1e-12, "a two-sided surface takes the absolute cosine");
    Check(light.Surface({0, 0, -150}, {0, 0, 1}, false) == 0, "nothing beyond the reach");
    light.effect = Effect::NonIncidence;
    Check(std::fabs(light.Surface({0, 0, 0}, {1, 0, 0}, false) - 0.5) < 1e-12, "NonIncidence is 1 - x whatever the angle");
    light.effect = Effect::Sunlight;
    light.direction = {0, 0, -1};
    Check(light.Surface({5000, 0, 0}, {0, 0, 1}, false) == 1, "a sun lights by angle alone");
    light.effect = Effect::Spotlight;
    light.direction = {0, 0, -1};
    light.cone = 64;    // k = 0.75: a point 99 units off the axis, 100 below, is outside
    Check(light.Surface({0, 0, 0}, {0, 0, 1}, false) > 0.49, "on a spot's axis");
    Check(light.Surface({99, 0, 0}, {0, 0, 1}, false) == 0, "outside a spot's cone");

    const Vec3 white = HsvColour(0, 255, 255);
    Check(std::fabs(white.x - 0.8213) < 1e-9 && white.x == white.y && white.y == white.z, "saturation 255 is white at 0.8213");
    const Vec3 red = HsvColour(0, 0, 255);
    Check(red.x > 0.8 && red.y == 0 && red.z == 0, "hue 0 unsaturated is red");
    const Vec3 ambient = LightmapAmbientBytes(0, 255, 255);
    Check(ambient.x == std::floor(0.8213 / 0.8213 * (1.4 * 0.7 / (0.01 + std::sqrt(1.4))) * 127.5), "ambient is half of FGetHSV, truncated");

    Light cook;
    Check(cook.CookDrops() && cook.Baked(), "a plain static light bakes and is dropped by the cook");
    cook.inGame = true;
    Check(!cook.CookDrops(), "an in-game light is kept");
    cook = Light{};
    cook.heatRadius = 1; cook.heatIntensity = 1;
    Check(!cook.CookDrops(), "a light with heat is kept");
    cook = Light{};
    cook.effect = Effect::SpotShadow;
    Check(!cook.CookDrops() && !cook.Baked(), "an Echelon spot shadow light is kept and does not bake");
    cook = Light{};
    cook.type = 0;
    Check(!cook.CookDrops() && !cook.Baked(), "a light with no type is not a light");

    SeededRandom random(42);
    Check(random.Next(0, 100) == 66, "the seeded generator draws as .NET's does");

    double brightness = 0;
    int hue = 0, saturation = 0;
    ColourOf(HsvColour(85, 40, 120) * 255, 1, brightness, hue, saturation);
    Check(hue == 85 && saturation == 40 && std::fabs(brightness - 120) < 0.01, "a colour gives back its hue, saturation and brightness");
}

// A 512-unit square room: floor at z 0 and one wall at x -256, each on its
// own half of a 128-texel atlas, 4 units a texel.
struct Room
{
    std::vector<std::uint32_t> pixels = std::vector<std::uint32_t>(128 * 128);
    Scene scene;

    void Build(const std::vector<Light>& bakedWith, double thin)
    {
        const AtlasImage image{128, 128, pixels.data()};
        SampleBuilder builder(thin);
        builder.AddPolygon({{{-256, -128, 0}, 0, 0}, {{256, -128, 0}, 1, 0}, {{256, 128, 0}, 1, 0.5}, {{-256, 128, 0}, 0, 0.5}},
                           {0, 0, 1}, false, 1, image, 0);
        builder.AddPolygon({{{-256, -128, 0}, 0, 0.5}, {{-256, 128, 0}, 0.5, 0.5}, {{-256, 128, 256}, 0.5, 1}, {{-256, -128, 256}, 0, 1}},
                           {1, 0, 0}, false, 1, image, 0);
        scene.samples = std::move(builder.samples);
        scene.ambient = [](int) { return Vec3{6, 6, 6}; };
        // A pillar's shadow: nothing passes through the box round (100, 0).
        scene.occluders.clear = [](const Vec3& a, const Vec3& b)
        {
            for (int step = 0; step <= 64; ++step)
            {
                const Vec3 p = a + (b - a) * (step / 64.0);
                if (std::fabs(p.x - 100) < 12 && std::fabs(p.y) < 12 && p.z < 80) return false;
            }
            return true;
        };
        scene.samples.atlas = BakeAt(scene, bakedWith);
    }
};

void Sampling()
{
    Room room;
    room.Build({}, 0);
    // 128 x 64 floor texels and 64 x 64 wall texels, every centre inside.
    Check(room.scene.samples.Count() == 128 * 64 + 64 * 64, "one sample per texel centre on a polygon");
    const Samples& s = room.scene.samples;
    Check(std::fabs(s.position[0].x + 254) < 1e-9 && std::fabs(s.position[0].y + 126) < 1e-9 && s.position[0].z == 0,
          "a texel centre maps to its place on the polygon");
    Check(s.atlas[0] == 6 && s.zone[0] == 1, "ambient alone where no light reaches");
    Room thin;
    thin.Build({}, 24);
    Check(thin.scene.samples.Count() < room.scene.samples.Count() / 20 && thin.scene.samples.Count() > 300, "thinned to one a cell");
}

void Fitting()
{
    Light lost;
    lost.name = "Lost";
    lost.location = {-60, 20, 140};
    lost.lightRadius = 15;          // reach 400
    lost.brightness = 110;
    lost.hue = 30;
    lost.saturation = 120;
    Light kept;
    kept.name = "Kept";
    kept.location = {180, -60, 90};
    kept.lightRadius = 9;
    kept.brightness = 80;
    kept.inGame = true;
    Room room;
    room.Build({lost, kept}, 12);
    const Scene& scene = room.scene;

    const auto knownBake = BakeAt(scene, {kept});
    const BakeError before = Compare(scene.samples, knownBake);
    Check(before.mae > kFitGateError && before.bias < 0, "without the lost light the rebake is darker than the lightmaps");
    std::vector<float> residual = Residual(scene.samples, knownBake);

    const SampleGrid grid(scene.samples.position, 256);
    double held = 0, predicted = 0;
    Check(Support(scene, grid, residual, lost, held, predicted) > 0.95, "the lightmaps hold the lost light");
    Light moved = lost;
    moved.location = {200, 100, 140};
    held = predicted = 0;
    Check(Support(scene, grid, residual, moved, held, predicted) < 0.6, "and little of a light that was not there");

    FitSettings settings;
    settings.thin = 12;
    LightFitter fitter(scene, residual, settings);
    std::vector<float> knownLight(residual.size());
    for (std::size_t i = 0; i < residual.size(); ++i) knownLight[i] = scene.samples.atlas[i] - 6 - residual[i];
    double measured = 0, share = 0, excess = 0;
    const double floor = fitter.NoiseFloor(knownLight, measured);
    fitter.Structure(floor, share, excess);
    Check(floor == settings.stopResidual && share > kFitGateShare, "the residual is structure, not noise");
    const auto fitted = fitter.Run(floor);
    Check(!fitted.empty() && fitted.size() <= 3, "a light or two explain it");
    const Vec3 off = fitted[0].location - lost.location;
    Check(Length(off) < 64, "the first stand-in sits by the lost light");
    Check(std::fabs(fitted[0].radius - lost.Radius()) < 120, "with about its reach");
    Check(fitted[0].support > 0.8, "and the residual held its light");

    std::vector<Light> all{kept};
    for (const auto& light : fitted)
    {
        Light standIn;
        standIn.location = light.location;
        standIn.lightRadius = light.radius / 25 - 1;
        ColourOf(light.amplitude, 1, standIn.brightness, standIn.hue, standIn.saturation);
        all.push_back(standIn);
    }
    const BakeError after = Compare(scene.samples, BakeAt(scene, all));
    Check(after.mae < before.mae / 4 && after.mae < 2, "a rebake with the stand-ins is close to the lightmaps");
    const std::string actor = StandInActor(fitted[0], 1, "RecoveredLight0");
    Check(actor.find("Begin Actor Class=Light Name=RecoveredLight0\n") == 0 && actor.find("LightRadius=") != std::string::npos
          && actor.find("Location=(X=") != std::string::npos && actor.rfind("End Actor\n") == actor.size() - 10, "the stand-in as an actor");

    // A map whose lights are all there: nothing to fit.
    Room whole;
    whole.Build({kept}, 12);
    const auto wholeResidual = Residual(whole.scene.samples, BakeAt(whole.scene, {kept}));
    LightFitter none(whole.scene, wholeResidual, settings);
    none.Structure(settings.stopResidual, share, excess);
    Check(share == 0 && none.Run(settings.stopResidual).empty(), "no residual, no stand-ins");
}

void Twins()
{
    using namespace RecoveredLightTwin;
    // Two atlases among other bytes: 16 x 16 and 32 x 16.
    std::vector<unsigned char> package(300, 0xAB);
    auto atlas = [&](int width, int height, unsigned char fill)
    {
        const int count = width * height * 4;
        for (int i = 0; i < 4; ++i) package.push_back(0);
        package.push_back(static_cast<unsigned char>(0x40 | (count & 0x3F)));
        int rest = count >> 6;
        while (rest >= 0x80) { package.push_back(static_cast<unsigned char>(0x80 | (rest & 0x7F))); rest >>= 7; }
        package.push_back(static_cast<unsigned char>(rest));
        for (int i = 0; i < count; ++i) package.push_back(static_cast<unsigned char>(fill + i % 7));
        for (int byte : {0, 0, 0, 0, 0, 5}) package.push_back(static_cast<unsigned char>(byte));
        for (int value : {width, height})
            for (int i = 0; i < 4; ++i) package.push_back(static_cast<unsigned char>((value >> (8 * i)) & 255));
        for (int i = 0; i < 50; ++i) package.push_back(0x40);
    };
    atlas(16, 16, 10);
    atlas(32, 16, 90);
    const auto found = FindAtlases(package.data(), package.size());
    Check(found.size() == 2 && found[0].width == 16 && found[1].width == 32 && found[1].height == 16, "both atlases are found");
    Check(found[0].hash != found[1].hash && found[0].hash.size() == 32, "each with its own hash");
    Check(FindAtlases(package.data(), 300).empty(), "none in bytes that hold none");
    const Fingerprint print = FingerprintOf(found);
    Check(SameBake(print, print) && !SameBake(print, {}) && !SameBake({}, {}), "a bake matches itself; no atlases match nothing");
    Check(Shared(print, {print[0]}) == 1, "atlases in common");

    std::vector<Entry> entries = {{"C:\\a\\MapsEd\\Old.sdc", 10, 5, "Old", {print[0]}},
                                  {"C:\\a\\MapsEd\\Other.sdc", 11, 9, "Other", print},
                                  {"C:\\b\\MapsEd\\Deck.sdc", 12, 7, "Deck", print},
                                  {"C:\\b\\MapsEd\\Deck2.sdc", 13, 8, "Deck", {"x"}}};
    const auto exact = FindExact(entries, print, "deck");
    Check(exact.size() == 2 && exact[0].package == "Deck" && exact[1].package == "Other", "the copy of the same name first");
    const auto stale = FindStale(entries, print, "deck");
    Check(stale.size() == 2 && stale[0].package == "Old" && stale[1].path.find("Deck2") != std::string::npos,
          "another bake: an atlas in common first, then the same name");
    const auto again = ReadCache(WriteCache(entries));
    Check(again.size() == 4 && again[1].fingerprint == print && again[2].size == 12 && again[3].modified == 8, "the cache reads back");

    const std::string map = "Begin Map\nBegin Actor Class=Light Name=Light3\n    LightRadius=12.000000\n    Level=LevelInfo'MyLevel.LevelInfo1'\nEnd Actor\n"
        "Begin Actor Class=Brush Name=Brush5\n    Begin Brush Name=Model6\n       LightRadius=1\n    End Brush\nEnd Actor\nEnd Map\n";
    const auto actors = Actors(map);
    Check(actors.size() == 2 && actors[0].className == "Light" && actors[0].name == "Light3" && actors[1].name == "Brush5", "actors of a map export");
    Check(SetsProperty(actors[0], "LightRadius") && !SetsProperty(actors[0], "LightHue") && !SetsProperty(actors[1], "LightRadius"),
          "a property the actor itself sets");
    Check(ReplaceName(actors[0].text, "LevelInfo1", "LevelInfo0").find("MyLevel.LevelInfo0'") != std::string::npos
          && ReplaceName("LevelInfo12 levelinfo1", "LevelInfo1", "X") == "LevelInfo12 X", "a name is replaced as a whole word");
    std::vector<std::string> used{"recoveredlight0"};
    Check(UniqueName("RecoveredLight0", used) == "RecoveredLight0_1" && UniqueName("RecoveredLight1", used) == "RecoveredLight1", "unique names");
}

int main()
{
    try
    {
        Terms();
        Sampling();
        Fitting();
        Twins();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "RecoveredLightModel: " << checks << " checks passed\n";
    return 0;
}
