#pragma once
#include "MapAuthoringModel.h"
#include <map>
#include <set>
#include <string>
#include <vector>

// File > Compare with Backup: what differs between the open map and a saved copy of it (a backup, an autosave or
// any .sdc), and the Map JSON change file that brings ticked differences back. The editor side
// (CompareBackupNative.inl) loads the copy under a package name of its own, reads both levels' actors the way Map
// JSON export reads them, and hands the two lists here. Each entry:
//   key        the object's path inside its level ("StaticMeshActor12", "Emitter3.SpriteEmitter0"),
//   actor      its identity {path, class} (a backup's paths are rewritten to the open level's package),
//   values     its exported values, as InspectActor reads them,
//   appearance StaticMesh, DrawType, DrawScale and DrawScale3D (actors that are not brushes),
//   team       a player start's TeamNumber, which only a re-added start carries,
//   brush      a brush (its shape is not compared and cannot be re-added),
//   owner      for a particle component, its emitter's key,
//   creatable  (backup entries) Map JSON can create its class.
namespace Workflow::Compare
{
    // The path inside the level: the level package's name and its dot taken off.
    inline std::string Key(const std::string& path,const std::string& package)
    {
        const auto prefix=package+".";
        return path.size()>prefix.size() && Fold(path.substr(0,prefix.size()))==Fold(prefix)?path.substr(prefix.size()):path;
    }
    // A loaded copy names its own objects by its own package; the open map's names use the open level's. Rewrites
    // object references ("Class'Copy.Thing'", a bare "Copy.Thing") in every string of a value.
    inline std::string RenameText(const std::string& text,const std::string& from,const std::string& to)
    {
        if(from.empty())return text;
        const auto lower=Fold(text),needle=Fold(from)+".";
        std::string out;size_t at=0;
        for(size_t i=0;i<text.size();)
        {
            const bool start=i==0 || text[i-1]=='\'' || text[i-1]=='"';
            if(start && lower.compare(i,needle.size(),needle)==0)
            {
                out+=text.substr(at,i-at)+to+".";i+=needle.size();at=i;continue;
            }
            ++i;
        }
        return out+text.substr(at);
    }
    inline Json Rename(const Json& value,const std::string& from,const std::string& to)
    {
        if(value.is_string())return RenameText(value.get<std::string>(),from,to);
        if(value.is_array()){Json out=Json::array();for(const auto& v:value)out.push_back(Rename(v,from,to));return out;}
        if(value.is_object()){Json out=Json::object();for(auto it=value.begin();it!=value.end();++it)out[it.key()]=Rename(it.value(),from,to);return out;}
        return value;
    }
    // The object path a reference value names: Class'Path' (quotes inside allowed), or nothing.
    inline std::string Referenced(const std::string& text)
    {
        const auto first=text.find('\''),last=text.rfind('\'');
        if(first==std::string::npos || first==0 || last!=text.size()-1 || first==last || text.find('\'',first+1)!=last)return {};
        auto name=text.substr(first+1,last-first-1);
        if(name.size()>=2 && name.front()=='"' && name.back()=='"')name=name.substr(1,name.size()-2);
        return name;
    }

    inline std::string Change(const std::string& property){return property=="Location" || property=="Rotation"?"Moved":"Changed";}
    inline std::string Where(const Json& entry)
    {
        const auto& values=entry.at("values");
        return values.contains("Location")?" at "+Magic::Text(values.at("Location")):std::string{};
    }
    // Every property of an entry the comparison reads: its values and its look.
    inline Json Properties(const Json& entry)
    {
        Json all=entry.value("values",Json::object());
        const auto look=entry.value("appearance",Json::object());
        for(auto it=look.begin();it!=look.end();++it)all[it.key()]=it.value();
        return all;
    }
    inline std::string Restored(const std::string& key,const std::string& name)
    {
        // Brought back, an actor whose name the deleted one still holds (kept for Undo) gets name_2, name_3...
        if(name.size()<=key.size()+1 || Fold(name.substr(0,key.size()+1))!=Fold(key+"_"))return {};
        return name.find_first_not_of("0123456789",key.size()+1)==std::string::npos?key:std::string{};
    }

    // What a row click selects: the actor, or a particle component's emitter.
    inline Json SelectFor(const Json& entry,const Json& current)
    {
        const auto owner=Fold(entry.value("owner",std::string{}));
        if(owner.empty())return entry.at("actor");
        for(const auto& e:current)if(Fold(e.at("key").get<std::string>())==owner)return e.at("actor");
        return nullptr;
    }
    // References to renamed objects (folded old path -> new path) rewritten to the new path.
    inline Json Follow(const Json& value,const std::map<std::string,std::string>& renamed)
    {
        if(renamed.empty())return value;
        if(value.is_array()){Json out=Json::array();for(const auto& v:value)out.push_back(Follow(v,renamed));return out;}
        if(value.is_object()){Json out=Json::object();for(auto it=value.begin();it!=value.end();++it)out[it.key()]=Follow(it.value(),renamed);return out;}
        if(!value.is_string())return value;
        const auto& text=value.get<std::string>();const auto path=Referenced(text);
        const auto found=path.empty()?renamed.end():renamed.find(Fold(path));
        return found==renamed.end()?value:Json(text.substr(0,text.find('\'')+1)+found->second+"'");
    }
    // Rows: change (Deleted, Added, Moved, Changed), target (the key), property, before (the backup's value), after (the
    // open map's), select (the open map's actor a click selects, or null), action ("create", "delete", "update" or
    // "" when it cannot be brought back), key, backupKey, note.
    inline Json Row(const std::string& change,const std::string& target,const Json& select,const std::string& property,const std::string& before,const std::string& after,
                    const std::string& action,const std::string& key,const std::string& backupKey,const std::string& note={})
    {
        return {{"change",change},{"target",target},{"select",select},{"property",property},{"before",before},{"after",after},
                {"action",action},{"key",key},{"backupKey",backupKey},{"note",note}};
    }
    // Matched by key and class; an open-map actor named key_N stands for the backup's key when that one is gone
    // (it is what bringing it back makes).
    inline Json Diff(const Json& current,const Json& backup)
    {
        std::map<std::string,size_t> byKey;
        for(size_t i=0;i<current.size();++i)byKey.emplace(Fold(current[i].at("key").get<std::string>())+"|"+current[i].at("actor").at("class").get<std::string>(),i);
        std::vector<int> partner(backup.size(),-1);std::vector<bool> used(current.size(),false);
        for(size_t i=0;i<backup.size();++i)
            if(auto found=byKey.find(Fold(backup[i].at("key").get<std::string>())+"|"+backup[i].at("actor").at("class").get<std::string>());found!=byKey.end() && !used[found->second])
            {partner[i]=static_cast<int>(found->second);used[found->second]=true;}
        for(size_t i=0;i<backup.size();++i)if(partner[i]<0)
            for(size_t j=0;j<current.size();++j)
                if(!used[j] && current[j].at("actor").at("class")==backup[i].at("actor").at("class") && !Restored(backup[i].at("key"),current[j].at("key")).empty())
                {partner[i]=static_cast<int>(j);used[j]=true;break;}
        // The particle components of an emitter matched under another name: by class, in order.
        std::map<std::string,std::string> owners;
        for(size_t i=0;i<backup.size();++i)if(partner[i]>=0)owners[Fold(backup[i].at("key").get<std::string>())]=Fold(current[partner[i]].at("key").get<std::string>());
        for(size_t i=0;i<backup.size();++i)if(partner[i]<0 && !backup[i].value("owner",std::string{}).empty())
        {
            const auto owner=owners.find(Fold(backup[i].at("owner").get<std::string>()));if(owner==owners.end())continue;
            for(size_t j=0;j<current.size();++j)
                if(!used[j] && Fold(current[j].value("owner",std::string{}))==owner->second && current[j].at("actor").at("class")==backup[i].at("actor").at("class"))
                {partner[i]=static_cast<int>(j);used[j]=true;break;}
        }
        // A backup reference to an object matched under another name names it as the open map does.
        std::map<std::string,std::string> renamed;
        for(size_t i=0;i<backup.size();++i)
            if(partner[i]>=0 && Fold(backup[i].at("actor").at("path").get<std::string>())!=Fold(current[partner[i]].at("actor").at("path").get<std::string>()))
                renamed[Fold(backup[i].at("actor").at("path").get<std::string>())]=current[partner[i]].at("actor").at("path").get<std::string>();
        Json deleted=Json::array(),added=Json::array(),moved=Json::array(),changed=Json::array();
        std::set<std::string> deletedKeys;
        for(size_t i=0;i<backup.size();++i)if(partner[i]<0)deletedKeys.insert(Fold(backup[i].at("key").get<std::string>()));
        for(size_t i=0;i<backup.size();++i)
        {
            const auto& old=backup[i];const auto key=old.at("key").get<std::string>();
            if(partner[i]<0)
            {
                std::string action="create",note;
                const auto owner=old.value("owner",std::string{});
                if(!owner.empty())
                {
                    action.clear();
                    note=deletedKeys.count(Fold(owner))?"comes back with "+owner:"a particle component on its own is not brought back";
                }
                else if(old.value("brush",false)){action.clear();note="a brush's shape is not kept in the comparison, so it cannot be brought back";}
                else if(!old.value("creatable",false)){action.clear();note="Map JSON cannot create this class";}
                deleted.push_back(Row("Deleted",key,nullptr,"class",old.at("actor").at("class").get<std::string>()+Where(old),{},action,{},key,note));
                deleted.back()["part"]=!owner.empty();
                continue;
            }
            const auto& now=current[partner[i]];const auto nowKey=now.at("key").get<std::string>();
            const auto before=Follow(Properties(old),renamed),after=Properties(now);
            std::set<std::string> names;for(auto it=before.begin();it!=before.end();++it)names.insert(it.key());for(auto it=after.begin();it!=after.end();++it)names.insert(it.key());
            for(const auto& name:names)
            {
                const bool had=before.contains(name),has=after.contains(name);
                // Both are of one class: a property only one side has is one Map JSON could not read on the other
                // (an array over its limits, say), not a change.
                if(!had || !has || before.at(name)==after.at(name))continue;
                const auto change=Change(name);
                // Brought back through the same Map JSON update. A component the map's package holds (not its
                // emitter) is listed by the emitter, and Map JSON reaches only owned ones; it selects its emitter.
                const bool shared=old.value("shared",false) || now.value("shared",false);
                auto row=Row(change,nowKey,SelectFor(now,current),name,Magic::Text(before.at(name)),Magic::Text(after.at(name)),shared?"":"update",nowKey,key,
                             shared?"a particle component the emitter does not own cannot be set back":"");
                row["part"]=!now.value("owner",std::string{}).empty();
                (change=="Moved"?moved:changed).push_back(std::move(row));
            }
        }
        for(size_t j=0;j<current.size();++j)if(!used[j])
        {
            const auto& now=current[j];const auto key=now.at("key").get<std::string>();
            const bool component=!now.value("owner",std::string{}).empty();
            added.push_back(Row("Added",key,SelectFor(now,current),"class",{},now.at("actor").at("class").get<std::string>()+Where(now),component?"":"delete",key,{},
                                component?"a particle component on its own is not deleted":"bringing the backup back deletes it"));
            added.back()["part"]=component;
        }
        auto byTarget=[](const Json& a,const Json& b){return Fold(a.at("target").get<std::string>())<Fold(b.at("target").get<std::string>());};
        Json rows=Json::array();
        for(auto* group:{&deleted,&added,&moved,&changed})
        {
            std::stable_sort(group->begin(),group->end(),byTarget);
            for(auto& row:*group)rows.push_back(std::move(row));
        }
        return rows;
    }
    // Distinct actors per change, for the window's heading.
    inline std::string Tally(const Json& rows)
    {
        std::map<std::string,std::set<std::string>> seen;
        // Particle components count with their emitters.
        for(const auto& row:rows)if(!row.value("part",false))seen[row.at("change").get<std::string>()].insert(Fold(row.at("target").get<std::string>()));
        std::string text;
        for(const char* change:{"Deleted","Added","Moved","Changed"})
            if(auto n=seen[change].size())
            {
                if(!text.empty())text+=", ";
                text+=std::to_string(n)+" "+(std::string(change)=="Deleted"?"deleted":std::string(change)=="Added"?"added":std::string(change)=="Moved"?"moved":"changed");
            }
        return text.empty()?"No differences":text;
    }

    // A creation ID for a re-added actor: its old name when that is a valid ID no open-map actor uses as name or Tag.
    inline std::string CreationId(const std::string& key,const std::set<std::string>& taken)
    {
        std::string id;
        for(char c:key)id+=std::isalnum(static_cast<unsigned char>(c)) || c=='_'?c:'_';
        if(id.empty() || std::isdigit(static_cast<unsigned char>(id[0])))id="_"+id;
        if(id.size()>56)id.resize(56);
        auto candidate=id;
        for(int n=2;taken.count(Fold(candidate));++n)candidate=id+"_"+std::to_string(n);
        return candidate;
    }
    struct Context
    {
        std::string level;                        // the open level's package ("MyLevel")
        std::map<std::string,std::string> ids;    // folded backup key -> creation ID, for actors brought back
        std::set<std::string> live,gone,removed;  // folded keys: in the open map; in the backup only; deleted by this file
        std::map<std::string,std::string> renamed; // folded backup key -> the open map's key_N that stands for it
    };
    // References inside a brought-back value: to an actor this file re-adds, by its creation ID; to one that is gone
    // and stays gone, or that this file deletes, None; anything else as it is.
    inline Json Relink(const Json& value,const Context& context)
    {
        if(value.is_array()){Json out=Json::array();for(const auto& v:value)out.push_back(Relink(v,context));return out;}
        if(value.is_object()){Json out=Json::object();for(auto it=value.begin();it!=value.end();++it)out[it.key()]=Relink(it.value(),context);return out;}
        if(!value.is_string())return value;
        const auto path=Referenced(value.get<std::string>());if(path.empty())return value;
        const auto prefix=Fold(context.level)+".";
        if(Fold(path).compare(0,prefix.size(),prefix)!=0)return value;
        const auto key=Fold(path.substr(prefix.size()));
        if(auto id=context.ids.find(key);id!=context.ids.end())return Json{{"$ref",id->second}};
        if(auto other=context.renamed.find(key);other!=context.renamed.end() && !context.live.count(key) && !context.removed.count(Fold(other->second)))
        {
            const auto& text=value.get<std::string>();
            return text.substr(0,text.find('\'')+1)+context.level+"."+other->second+"'";
        }
        if(context.removed.count(key) || (context.gone.count(key) && !context.live.count(key)))return "None";
        return value;
    }
    // The Map JSON change file (version 3) for the ticked rows. A Deleted row re-adds the actor with the backup's
    // values (and an emitter's particle components); an Added row deletes the actor; a Moved or Changed row sets
    // that property back. Rows that cannot be brought back are skipped; nothing to do throws.
    inline Json Changes(const Json& comparison,const std::vector<size_t>& ticked,const std::string& map,const std::string& description)
    {
        const auto& rows=comparison.at("rows");const auto& current=comparison.at("current");const auto& backup=comparison.at("backup");
        std::map<std::string,const Json*> now,old;
        for(const auto& e:current)now[Fold(e.at("key").get<std::string>())]=&e;
        for(const auto& e:backup)old[Fold(e.at("key").get<std::string>())]=&e;
        Context context;context.level=comparison.at("level").get<std::string>();
        std::set<std::string> taken;
        for(const auto& e:current){context.live.insert(Fold(e.at("key").get<std::string>()));taken.insert(Fold(e.at("key").get<std::string>()));if(e.contains("tag"))taken.insert(Fold(e.at("tag").get<std::string>()));}
        for(const auto& e:backup)if(!context.live.count(Fold(e.at("key").get<std::string>())))
        {
            const auto key=e.at("key").get<std::string>();context.gone.insert(Fold(key));
            for(const auto& c:current)
                if(c.at("actor").at("class")==e.at("actor").at("class") && !Restored(key,c.at("key")).empty()){context.renamed[Fold(key)]=c.at("key").get<std::string>();break;}
        }
        std::vector<std::string> creates,deletes;std::map<std::string,Json> updates;std::vector<std::string> updateOrder;
        for(auto index:ticked)
        {
            if(index>=rows.size())throw std::runtime_error("The comparison has no row "+std::to_string(index)+".");
            const auto& row=rows[index];const auto action=row.at("action").get<std::string>();
            if(action=="create")
            {
                const auto key=Fold(row.at("backupKey").get<std::string>());
                if(!context.ids.count(key)){auto id=CreationId(row.at("backupKey"),taken);taken.insert(Fold(id));context.ids[key]=id;creates.push_back(key);}
            }
            else if(action=="delete")
            {
                const auto key=Fold(row.at("key").get<std::string>());
                if(context.removed.insert(key).second)deletes.push_back(key);
            }
            else if(action=="update")
            {
                const auto key=Fold(row.at("key").get<std::string>());
                if(!updates.count(key)){updates[key]=Json::object();updateOrder.push_back(key);}
                updates[key][row.at("property").get<std::string>()]=row.at("backupKey").get<std::string>();
            }
        }
        if(creates.empty() && deletes.empty() && updates.empty())throw std::runtime_error("Tick at least one row that can be brought back.");
        Json ops=Json::array();
        for(const auto& key:deletes)
        {
            const auto& e=*now.at(key);
            ops.push_back({{"op","delete"},{"actor",e.at("actor")},{"before",e.at("values")}});
        }
        for(const auto& key:creates)
        {
            const auto& e=*old.at(key);const auto& id=context.ids.at(key);
            bool parts=false;
            for(const auto& c:backup)if(Fold(c.value("owner",std::string{}))==key && c.value("creatable",false))parts=true;
            Json properties=Json::object();
            for(auto it=e.at("values").begin();it!=e.at("values").end();++it)
                if(!(parts && it.key()=="Emitters"))properties[it.key()]=Relink(it.value(),context);
            const auto look=e.value("appearance",Json::object());
            for(auto it=look.begin();it!=look.end();++it)properties[it.key()]=Relink(it.value(),context);
            if(e.contains("team"))properties["TeamNumber"]=e.at("team");
            ops.push_back({{"op","create"},{"id",id},{"class",e.at("actor").at("class")},{"geometry","point"},{"properties",properties}});
            for(const auto& c:backup)if(Fold(c.value("owner",std::string{}))==key && c.value("creatable",false))
            {
                auto part=c.at("key").get<std::string>();part=part.substr(part.find_last_of('.')+1);
                const auto partId=CreationId(id+"_"+part,taken);taken.insert(Fold(partId));
                ops.push_back({{"op","component"},{"id",partId},{"class",c.at("actor").at("class")},{"owner",id},{"properties",Relink(c.at("values"),context)}});
            }
        }
        for(const auto& key:updateOrder)
        {
            const auto& e=*now.at(key);Json properties=Json::object();
            for(auto it=updates[key].begin();it!=updates[key].end();++it)
            {
                const auto& source=*old.at(Fold(it.value().get<std::string>()));const auto all=Properties(source);
                properties[it.key()]=Relink(all.at(it.key()),context);
            }
            ops.push_back({{"op","update"},{"actor",e.at("actor")},{"before",e.at("values")},{"properties",properties}});
        }
        Json document={{"format","scct.map-changes"},{"version",Authoring::kLatestVersion},{"map",map},{"description",description},{"operations",ops}};
        if(!deletes.empty())document["allowDeletes"]=true;
        return document;
    }
}
