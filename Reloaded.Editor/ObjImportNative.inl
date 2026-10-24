// Import OBJ (ObjImportModel.h): reads the OBJ and its MTL files, finds or imports each
// material, writes each part as an ASE file and runs the stock STATICMESH IMPORT on it, checks
// what the editor made and places the parts. Included by WorkflowEditor.cpp.
namespace
{
    namespace fs = std::filesystem;
    std::string ObjUtf8(const fs::path& path) { const auto text = path.u8string(); return std::string(text.begin(), text.end()); }
    struct ObjSlot
    {
        std::string name;        // the OBJ's usemtl name ("" before any)
        std::string kind;        // "loaded", "image" or "default"
        Address material = 0;    // loaded or default material (an image's after import)
        fs::path image;          // map_Kd file, for "image"
        std::string texture;     // the object name an image is imported as
        std::string note;
    };
    struct ObjPlan
    {
        ObjImport::Built built;
        std::vector<ObjSlot> slots;
        std::string package, group, name, textureGroup;
        size_t limit = ObjImport::kCollisionTriangles;
        bool split = true, place = false;
        ObjImport::Collision collision = ObjImport::Collision::Parts;
        std::vector<std::string> warnings;
    };
    std::string ObjText(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read " + ObjUtf8(file));
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.size() > 512u * 1024u * 1024u) throw std::runtime_error(ObjUtf8(file) + " is too large.");
        return text;
    }
    ObjImport::Collision ObjCollision(const std::string& name)
    {
        using ObjImport::Collision;
        if (name == "none") return Collision::None;
        if (name == "box") return Collision::Box;
        if (name == "polygons") return Collision::Polygons;
        if (name == "groups") return Collision::Groups;
        if (name == "parts") return Collision::Parts;
        if (name == "file") return Collision::File;
        throw std::runtime_error("Collision is none, box, polygons, groups, parts or file.");
    }
    // Every loaded material, by folded name.
    std::unordered_map<std::string, std::vector<Address>> ObjLoadedMaterials()
    {
        std::unordered_map<std::string, std::vector<Address>> out;
        std::unordered_map<Address, bool> isMaterial;
        for (auto at : Array(0x11697B70))
        {
            const auto a = Read<Address>(at);
            if (!a) continue;
            const auto type = Read<Address>(a + 0x24);
            if (!type) continue;
            // The class chain is read once per class, not once per object.
            auto known = isMaterial.find(type);
            if (known == isMaterial.end()) known = isMaterial.emplace(type, IsA(a, "Material")).first;
            if (!known->second) continue;
            out[Fold(NameOf(a))].push_back(a);
        }
        return out;
    }
    // A map_Kd file as the MTL names it: beside the MTL, or by its name alone beside the OBJ.
    fs::path ObjImageFile(const std::string& written, const fs::path& mtlFolder, const fs::path& objFolder)
    {
        std::string text = written;
        for (auto& c : text) if (c == '/') c = '\\';
        fs::path given = Authoring::Utf8Path(text);
        std::error_code error;
        for (const auto& candidate : {given.is_absolute() ? given : mtlFolder / given, objFolder / given.filename(), mtlFolder / given.filename()})
            if (fs::is_regular_file(candidate, error)) return candidate;
        return {};
    }
    ObjPlan PlanObjImport(const Json& spec)
    {
        Engine();
        ObjPlan plan;
        const fs::path file = Authoring::Utf8Path(spec.at("file").get<std::string>());
        if (file.empty()) throw std::runtime_error("Choose an OBJ file.");
        const auto parsed = ObjImport::ParseObj(ObjText(file));
        plan.warnings = parsed.warnings;
        // Materials from every MTL file it names.
        std::map<std::string, std::pair<ObjImport::Material, fs::path>> mtl;
        for (const auto& library : parsed.libraries)
        {
            std::vector<std::string> names{library};
            std::error_code error;
            if (!fs::is_regular_file(file.parent_path() / Authoring::Utf8Path(library), error))
            {
                // Several files on one line, separated by spaces.
                names.clear();
                std::istringstream words(library);
                for (std::string w; words >> w;) names.push_back(w);
            }
            for (const auto& name : names)
            {
                const auto path = file.parent_path() / Authoring::Utf8Path(name);
                if (!fs::is_regular_file(path, error)) { plan.warnings.push_back("The material file " + name + " is missing."); continue; }
                for (const auto& m : ObjImport::ParseMtl(ObjText(path))) mtl.emplace(m.name, std::pair{m, path.parent_path()});
            }
        }
        ObjImport::Options options;
        options.scale = spec.value("scale", 1.0);
        options.up = spec.value("up", std::string("y")) == "z" ? ObjImport::Up::Z : ObjImport::Up::Y;
        plan.collision = options.collision = ObjCollision(spec.value("collision", std::string("parts")));
        plan.name = spec.value("name", std::string());
        if (plan.name.empty()) plan.name = ObjImport::SafeName(ObjUtf8(file.stem()));
        if (!ObjImport::ValidName(plan.name)) throw std::runtime_error("Name the mesh with letters, digits and _ (at most 63), starting with a letter or _.");
        plan.package = spec.value("package", std::string("MyLevel"));
        if (!ObjImport::ValidName(plan.package)) throw std::runtime_error("Name the package with letters, digits and _.");
        if (Fold(plan.package) == "mylevel") plan.package = LevelPath();
        plan.group = spec.value("group", std::string());
        if (!plan.group.empty() && !ObjImport::ValidName(plan.group)) throw std::runtime_error("Name the group with letters, digits and _, or leave it empty.");
        plan.textureGroup = Fold(plan.group) == "textures" ? "Materials" : "Textures";
        plan.limit = static_cast<size_t>(std::clamp(spec.value("partTriangles", static_cast<int>(ObjImport::kCollisionTriangles)), 100,
                                                    static_cast<int>(ObjImport::kCollisionTriangles)));
        plan.split = spec.value("split", true);
        plan.place = spec.value("place", false);
        plan.built = ObjImport::Build(parsed, options, plan.name);
        plan.warnings.insert(plan.warnings.end(), plan.built.warnings.begin(), plan.built.warnings.end());

        // Each slot's material: a loaded one of that name, else the MTL's diffuse image, else the
        // engine's default texture, so the importer never asks for one.
        const auto loaded = ObjLoadedMaterials();
        Address fallback = Find("Engine.DefaultTexture");
        if (!fallback)
            for (const auto& [name, list] : loaded) { fallback = list.front(); break; }
        for (size_t i = 0; i < plan.built.mesh.slots.size(); ++i)
        {
            ObjSlot slot;
            slot.name = plan.built.mesh.slots[i];
            const auto safe = ObjImport::SafeName(slot.name, "Material");
            const auto found = slot.name.empty() ? loaded.end() : loaded.find(Fold(safe));
            const auto described = mtl.find(slot.name);
            if (found != loaded.end())
            {
                slot.kind = "loaded";
                slot.material = found->second.front();
                for (auto m : found->second)
                    if (Fold(Path(m)).rfind(Fold(plan.package) + ".", 0) == 0) slot.material = m;
                if (found->second.size() > 1) slot.note = std::to_string(found->second.size()) + " loaded materials have this name";
            }
            else if (described != mtl.end() && !described->second.first.diffuseMap.empty())
            {
                slot.image = ObjImageFile(described->second.first.diffuseMap, described->second.second, file.parent_path());
                if (slot.image.empty())
                {
                    slot.kind = "default";
                    slot.material = fallback;
                    slot.note = "its image " + described->second.first.diffuseMap + " is missing";
                }
                else { slot.kind = "image"; slot.texture = safe; }
            }
            else
            {
                slot.kind = "default";
                slot.material = fallback;
                slot.note = slot.name.empty() ? "the faces have no material" : described == mtl.end() ? "no loaded material has this name" : "the MTL gives it no image";
            }
            if (slot.kind == "default" && !fallback) throw std::runtime_error("No material is loaded to give the faces without one.");
            plan.slots.push_back(std::move(slot));
        }
        return plan;
    }
    std::vector<ObjImport::Mesh> ObjParts(const ObjPlan& plan, size_t limit)
    {
        if (ObjImport::WithinLimits(plan.built.mesh, limit)) return {plan.built.mesh};
        if (!plan.split)
            throw std::runtime_error(plan.built.mesh.name + " has " + std::to_string(plan.built.mesh.triangles.size()) + " triangles and " +
                                     std::to_string(ObjImport::HullTriangleCount(plan.built.mesh)) + " hull triangles: a mesh with more than " +
                                     std::to_string(ObjImport::kCollisionTriangles) + " of either crashes the game. Tick Split into parts.");
        return ObjImport::Split(plan.built.mesh, limit);
    }
    const char* ObjCollisionName(ObjImport::Collision c)
    {
        using ObjImport::Collision;
        switch (c)
        {
        case Collision::None: return "none";
        case Collision::Box: return "a box";
        case Collision::Polygons: return "its own triangles";
        case Collision::Groups: return "a hull per object or group";
        case Collision::Parts: return "a hull per connected part";
        default: return "the file's UCX_ objects";
        }
    }
    Json ObjSlotsJson(const ObjPlan& plan)
    {
        Json slots = Json::array();
        for (const auto& s : plan.slots)
        {
            Json row = {{"name", s.name}, {"kind", s.kind}, {"note", s.note}};
            if (s.material) row["material"] = Path(s.material);
            if (s.kind == "image") { row["image"] = ObjUtf8(s.image); row["texture"] = plan.package + "." + plan.textureGroup + "." + s.texture; }
            slots.push_back(row);
        }
        return slots;
    }
    // A power of two TGA, BMP, PCX or DDS goes to the importer as it is; anything else GDI+
    // reads (PNG, JPG...) is scaled to powers of two and written as a TGA first.
    Address ImportObjTexture(const ObjPlan& plan, const ObjSlot& slot, std::vector<std::string>& warnings)
    {
        const auto path = plan.package + "." + plan.textureGroup + "." + slot.texture;
        if (auto existing = Find(path); existing && IsA(existing, "Material")) return existing;
        const auto extension = Authoring::Extension(ObjUtf8(slot.image));
        fs::path source = slot.image, temporary;
        bool alpha = false, direct = false;
        std::pair<uint32_t, uint32_t> size{};
        if (CharacterSkins::Importable(extension))
        {
            std::ifstream input(slot.image, std::ios::binary);
            std::string header(64, '\0');
            input.read(header.data(), static_cast<std::streamsize>(header.size()));
            header.resize(static_cast<size_t>(input.gcount()));
            try
            {
                size = Authoring::ImageSize(header, extension);
                direct = true;
                alpha = extension == ".tga" && header.size() > 16 && static_cast<unsigned char>(header[16]) == 32;
            }
            catch (const std::exception&) {}
        }
        if (!direct)
        {
            char folder[MAX_PATH]{};
            GetTempPathA(MAX_PATH, folder);
            temporary = fs::path(folder) / ("RE_obj_" + slot.texture + ".tga");
            int width = 0, height = 0;
            std::string error;
            if (!ObjImportWindow::ImageToTga(slot.image, temporary, width, height, alpha, error))
            {
                warnings.push_back(ObjUtf8(slot.image.filename()) + ": " + error + "; the default texture is used instead.");
                return 0;
            }
            size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
            source = temporary;
        }
        Exec("TEXTURE IMPORT FILE=\"" + source.string() + "\" NAME=\"" + slot.texture + "\" PACKAGE=\"" + plan.package + "\" GROUP=\"" + plan.textureGroup + "\" MIPS=1");
        std::error_code ignored;
        if (!temporary.empty()) fs::remove(temporary, ignored);
        auto texture = Find(path);
        if (!texture || !IsA(texture, "Texture"))
        {
            warnings.push_back("The editor's texture importer refused " + ObjUtf8(slot.image.filename()) + "; the default texture is used instead.");
            return 0;
        }
        if (extension != ".dds" || !direct)
        {
            int chain = 1;
            for (auto side = (std::max)(size.first, size.second); side > 1; side >>= 1) ++chain;
            Exec("TEXTURE COMPRESS NAME=" + path + " FORMAT=" + std::string(alpha ? "DXT5" : "DXT1") + " MaxMips=" + std::to_string(chain));
        }
        return texture;
    }
    // A static mesh's materials array: (element address, element size, Material and
    // EnableCollision fields).
    struct ObjMaterialArray { Address data = 0; int count = 0, stride = 0; Address material = 0, collision = 0; };
    ObjMaterialArray ObjMeshMaterials(Address mesh)
    {
        ObjMaterialArray out;
        auto p = Property(mesh, "Materials");
        if (!p || NameOf(Read<Address>(p + 0x24)) != "ArrayProperty") return out;
        const auto inner = Read<Address>(p + 0x64);
        const auto type = inner ? Read<Address>(inner + 0x64) : 0;
        if (!type) return out;
        const auto at = mesh + Read<int>(p + 0x3c);
        out.data = Read<Address>(at);
        out.count = Read<int>(at + 4);
        out.stride = Read<unsigned short>(inner + 0x32);
        out.material = StructField(type, "Material");
        out.collision = StructField(type, "EnableCollision");
        if (out.count < 0 || out.count > 4096 || !out.stride || (out.count && !out.data)) out = {};
        return out;
    }
    Json ObjMeshReport(Address mesh)
    {
        // UStaticMesh (editor): per-polygon collision triangles +0x104 and their BSP +0x114,
        // hull triangles +0x148 and their BSP +0x150 (TArray: data, count, capacity).
        Json materials = Json::array();
        const auto array = ObjMeshMaterials(mesh);
        for (int i = 0; i < array.count; ++i)
        {
            const auto element = array.data + static_cast<Address>(i) * static_cast<Address>(array.stride);
            Json row = {{"material", array.material ? Path(Read<Address>(element + Read<int>(array.material + 0x3c))) : std::string()}};
            if (array.collision)
                row["collision"] = (Read<unsigned>(element + Read<int>(array.collision + 0x3c)) & Read<unsigned>(array.collision + 0x64)) != 0;
            materials.push_back(row);
        }
        const auto box = Read<std::array<float, 6>>(mesh + 0x28);
        return {{"path", Path(mesh)},
                {"polygonCollision", Read<int>(mesh + 0x108)},
                {"polygonNodes", Read<int>(mesh + 0x118)},
                {"hullTriangles", Read<int>(mesh + 0x14c)},
                {"hullNodes", Read<int>(mesh + 0x154)},
                {"bounds", Json::array({box[0], box[1], box[2], box[3], box[4], box[5]})},
                {"materials", materials}};
    }
}

Json ObjImportPreview(const Json& spec)
{
    const auto plan = PlanObjImport(spec);
    std::vector<ObjImport::Mesh> parts;
    std::string error;
    try { parts = ObjParts(plan, plan.limit); }
    catch (const std::exception& e) { error = e.what(); }
    Json rows = Json::array();
    for (const auto& p : parts)
        rows.push_back({{"name", p.name}, {"triangles", p.triangles.size()}, {"hulls", p.hulls.size()}, {"hullTriangles", ObjImport::HullTriangleCount(p)},
                        {"exists", Find(plan.package + "." + (plan.group.empty() ? "" : plan.group + ".") + p.name) != 0}});
    const auto bounds = ObjImport::Bounds(plan.built.mesh);
    return {{"name", plan.name}, {"package", plan.package}, {"group", plan.group},
            {"triangles", plan.built.mesh.triangles.size()}, {"hulls", plan.built.mesh.hulls.size()},
            {"hullTriangles", ObjImport::HullTriangleCount(plan.built.mesh)}, {"collision", ObjCollisionName(plan.collision)},
            {"size", Json::array({bounds[3] - bounds[0], bounds[4] - bounds[1], bounds[5] - bounds[2]})},
            {"slots", ObjSlotsJson(plan)}, {"parts", rows}, {"error", error}, {"warnings", plan.warnings}};
}

// Imports the OBJ as spec says: file, package (MyLevel: the open map), group, name, scale, up
// ("y" or "z"), collision (none, box, polygons, groups, parts, file), split, partTriangles and
// place. Returns what the editor made: each part's collision as the editor built it, the
// materials, the placed actors and warnings.
Json ImportObjMesh(const Json& spec)
{
    auto plan = PlanObjImport(spec);
    auto parts = ObjParts(plan, plan.limit);
    const auto prefix = plan.package + "." + (plan.group.empty() ? "" : plan.group + ".");
    for (const auto& p : parts)
        if (Find(prefix + p.name)) throw std::runtime_error(prefix + p.name + " exists already: choose another name, or delete it first.");
    std::vector<std::string> warnings = plan.warnings;
    for (auto& slot : plan.slots)
        if (slot.kind == "image")
        {
            slot.material = ImportObjTexture(plan, slot, warnings);
            if (!slot.material)
            {
                slot.kind = "default";
                slot.material = Find("Engine.DefaultTexture");
                if (!slot.material) throw std::runtime_error("No default texture is loaded.");
            }
        }
    // The importer finds each material by its name alone: give it the chosen object's name, and
    // point each slot at the chosen object afterwards in case another has the same name.
    std::vector<std::string> names;
    for (const auto& slot : plan.slots) names.push_back(NameOf(slot.material));
    char folder[MAX_PATH]{};
    GetTempPathA(MAX_PATH, folder);
    size_t limit = plan.limit;
    std::vector<Address> made;
    int attempts = 0;
    for (;;)
    {
        ++attempts;
        made.clear();
        for (const auto& source : parts)
        {
            auto part = source;
            // A part's slots are a subset of the mesh's: map them back by name.
            std::vector<int> slotOf;
            for (auto& s : part.slots)
            {
                const auto at = std::find(plan.built.mesh.slots.begin(), plan.built.mesh.slots.end(), s);
                const auto index = static_cast<size_t>(at - plan.built.mesh.slots.begin());
                slotOf.push_back(static_cast<int>(index));
                s = names.at(index);
            }
            const auto text = ObjImport::Ase(part);
            const auto file = fs::path(folder) / ("RE_obj_" + part.name + ".ase");
            {
                std::ofstream out(file, std::ios::binary);
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!out) throw std::runtime_error("Cannot write " + file.string());
            }
            Exec("STATICMESH IMPORT FILE=\"" + file.string() + "\" NAME=\"" + part.name + "\" PACKAGE=\"" + plan.package + "\"" +
                 (plan.group.empty() ? std::string() : " GROUP=\"" + plan.group + "\""));
            std::error_code ignored;
            fs::remove(file, ignored);
            const auto mesh = Find(prefix + part.name);
            if (!mesh || !IsA(mesh, "StaticMesh")) throw std::runtime_error("The editor did not import " + prefix + part.name + ".");
            made.push_back(mesh);
            // Each slot to the material chosen for it.
            const auto array = ObjMeshMaterials(mesh);
            if (array.material)
                for (int i = 0; i < array.count && i < static_cast<int>(slotOf.size()); ++i)
                    Write<Address>(array.data + static_cast<Address>(i) * static_cast<Address>(array.stride) + Read<int>(array.material + 0x3c),
                                   plan.slots[static_cast<size_t>(slotOf[static_cast<size_t>(i)])].material);
        }
        // A collision tree at the editor's 32767-node cap leaves triangles out of the
        // collision: smaller parts, if splitting is allowed.
        bool capped = false;
        for (auto mesh : made) capped = capped || Read<int>(mesh + 0x118) >= ObjImport::kBspNodes || Read<int>(mesh + 0x154) >= ObjImport::kBspNodes;
        if (!capped) break;
        if (!plan.split || attempts >= 4 || limit <= 400)
        {
            warnings.push_back("A collision tree reached the editor's 32767-node cap, so part of the collision is lost: import again with fewer triangles per part.");
            break;
        }
        for (auto mesh : made) Exec("DELETE CLASS=STATICMESH OBJECT=\"" + Path(mesh) + "\"");
        bool gone = true;
        for (const auto& p : parts) gone = gone && !Find(prefix + p.name);
        if (!gone)
        {
            warnings.push_back("A collision tree reached the editor's 32767-node cap and the parts could not be made again: delete them and import with fewer triangles per part.");
            break;
        }
        limit /= 2;
        parts = ObjImport::Split(plan.built.mesh, limit);
    }
    // No collision: no material collides, and the mesh's collision is built again without them.
    if (plan.collision == ObjImport::Collision::None)
        for (auto mesh : made)
        {
            const auto array = ObjMeshMaterials(mesh);
            if (!array.collision) { warnings.push_back("This editor does not expose EnableCollision; " + Path(mesh) + " still collides."); continue; }
            const auto mask = Read<unsigned>(array.collision + 0x64);
            for (int i = 0; i < array.count; ++i)
            {
                const auto at = array.data + static_cast<Address>(i) * static_cast<Address>(array.stride) + Read<int>(array.collision + 0x3c);
                Write<unsigned>(at, Read<unsigned>(at) & ~mask);
            }
            Call(mesh, 0x44);
        }
    Json result = {{"parts", Json::array()}, {"slots", ObjSlotsJson(plan)}, {"attempts", attempts}, {"partTriangles", limit}};
    for (size_t i = 0; i < made.size(); ++i)
    {
        auto row = ObjMeshReport(made[i]);
        row["triangles"] = parts[i].triangles.size();
        row["hulls"] = parts[i].hulls.size();
        row["expectedHullTriangles"] = ObjImport::HullTriangleCount(parts[i]);
        const auto expected = ObjImport::Bounds(parts[i]);
        const auto& got = row.at("bounds");
        for (int k = 0; k < 6; ++k)
            if (std::abs(got[static_cast<size_t>(k)].get<double>() - expected[static_cast<size_t>(k)]) > 0.5)
            {
                warnings.push_back(Path(made[i]) + " imported with bounds other than the file's.");
                break;
            }
        result["parts"].push_back(row);
    }
    if (plan.place && !made.empty())
    {
        // Where the viewport was last clicked; every part at the same place, one Undo step.
        auto unrealEd = Read<Address>(0x117a59b0);
        Vector at{};
        if (unrealEd)
        {
            const auto click = Read<std::array<float, 3>>(unrealEd + 0x1bc);
            for (int axis = 0; axis < 3; ++axis) at[static_cast<size_t>(axis)] = std::isfinite(click[static_cast<size_t>(axis)]) ? click[static_cast<size_t>(axis)] : 0.0f;
        }
        Json operations = Json::array();
        for (auto mesh : made)
            operations.push_back({{"op", "create"}, {"id", NameOf(mesh)}, {"class", "Engine.StaticMeshActor"},
                                  {"properties", {{"Location", {{"X", std::to_string(at[0])}, {"Y", std::to_string(at[1])}, {"Z", std::to_string(at[2])}}},
                                                  {"StaticMesh", "StaticMesh'" + Path(mesh) + "'"}}}});
        auto applied = ApplyMapAuthoring({{"format", "scct.map-changes"}, {"version", 1}, {"map", AuthoringMapKey()},
                                          {"description", "Place " + plan.name}, {"operations", operations}});
        result["placed"] = applied.at("created");
        Select(applied.at("created"));
        Redraw();
    }
    result["warnings"] = warnings;
    if (!made.empty()) StaticMeshBrowserFavorites::UseMesh(reinterpret_cast<void*>(made.front()));
    return result;
}
