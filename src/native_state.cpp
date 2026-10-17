#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_RAND_S // rand_s: RtlGenRandom-backed session nonce
#include <windows.h>
#include "native_state.h"
#include "game_offsets.h"
#include "host.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {
// Game slot offsets, method fingerprints and the legacy discovery pattern are
// generated per game build by xenomit-tooling-offset-updater. Offsets below
// that are not in `go` are Flash runtime internals pinned by kRuntimeSha256.
namespace go = xenomit::game_offsets;
// Exact xenomit-game-api live/layout.rs version 2 ABI.
struct Header {
    uint32_t magic, version, seq, pad;
    uint64_t frame, timestamp;
    uint32_t capture_us; int32_t map_id;
    float width,height; int32_t hero_id;
    float hero_x,hero_y,dest_x,dest_y;
    uint32_t hero_flags; int32_t faction,clan;
    uint32_t entities,strings; uint64_t navigation_revision;
    uint32_t navigation_off,navigation_len; int32_t target_id,attack_target_id,hero_speed; uint8_t reserved[12];
};
struct Entity {
    int32_t id; uint8_t kind,flags; uint16_t pad;
    float x,y,dx,dy;
    int32_t hp,max_hp,shield,max_shield,nano,max_nano,faction,clan,diplomacy,type;
    uint32_t name_off,name_len;
};
static_assert(sizeof(Header)==128 && offsetof(Header,seq)==8);
static_assert(sizeof(Entity)==72);
constexpr size_t Capacity=8192, MappingBytes=2*1024*1024;
constexpr size_t Strings=128+Capacity*sizeof(Entity);
// DarkBot's MainClientApplication value fingerprint, re-emitted per build at
// the current field offsets. Validate class identities and each decoded root.
static_assert(sizeof(go::kAppPattern)>=8 && go::kAppPatternMask[0]==0xff && go::kAppPatternOffset%4==0);
// NativeAnchor (bridge/DobotBridge.as) slots: the Object slot sits in the
// pointer area before the Number area, so screen=+32, tag0=+40, tag1=+48.
constexpr size_t AnchorScreen=32, AnchorTag0=40;
bool copy(void* to,uintptr_t from,size_t size) {
    if(from<0x10000 || from>0x00007fffffffffffULL-size)return false;
    __try {std::memcpy(to,reinterpret_cast<void*>(from),size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
template<class T> T read(uintptr_t p,bool& ok) {
    T v{};if(!copy(&v,p,sizeof(v)))ok=false;return v;
}
uintptr_t ptr(uintptr_t p,bool& ok) {return read<uintptr_t>(p,ok)&~uintptr_t(7);}
// AVM String: UTF-8 output from Latin-1/UTF-16 and dependent substrings.
bool string(uintptr_t p,std::string& out) {
    out.clear();if(!p)return true;
    bool ok=true;auto n=read<uint32_t>(p+32,ok),flags=read<uint32_t>(p+36,ok);
    if(!ok || n>1024 || ((flags&6)>>1)>2)return false;
    auto data=read<uintptr_t>(p+16,ok);
    if((flags&6)==4) {
        auto master=ptr(p+24,ok);auto masterFlags=read<uint32_t>(master+36,ok),masterLength=read<uint32_t>(master+32,ok);
        auto width=1+(flags&1);
        auto buffer=read<uintptr_t>(master+16,ok);
        uint64_t capacity=masterLength;
        if((masterFlags&6)==0) {
            // Adobe String::_append consumes CHARSLEFT without extending the
            // master's length. GC::Size, not length+remaining padding, bounds
            // the actual allocation. Validate the pinned MMgc block layout.
            auto block=buffer&~uintptr_t(4095),masterBlock=master&~uintptr_t(4095);
            auto size=read<uint32_t>(block+4,ok);
            auto gc=read<uintptr_t>(block+8,ok);
            auto masterGc=read<uintptr_t>(masterBlock+8,ok);
            if(!ok || !gc || gc!=masterGc || (buffer&7) || size<8 || size>8*1024*1024 || (size&7) || size%width || size/width<masterLength)return false;
            if(size<=2016) {
                auto items=read<uintptr_t>(block+80,ok);
                if(!ok || items<block+88 || items>=block+4096 || buffer<items || (buffer-items)%size || buffer+size>block+4096)return false;
            }
            capacity=size/width;
        }
        if(!ok||(masterFlags&6)==4||(masterFlags&6)==6||(masterFlags&1)!=(flags&1)||data%width||data/width>capacity||n>capacity-data/width)return false;
        data+=buffer;
    }
    if(!ok)return false;
    std::array<wchar_t,1024> wide{};std::array<uint8_t,1024> narrow{};
    if(flags&1) {if(!copy(wide.data(),data,n*2))return false;}
    else {if(!copy(narrow.data(),data,n))return false;for(uint32_t i=0;i<n;++i)wide[i]=narrow[i];}
    if(!n)return true;
    int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),n,nullptr,0,nullptr,nullptr);
    if(bytes<=0)return false;
    out.resize(bytes);return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),n,out.data(),bytes,nullptr,nullptr)==bytes;
}
bool class_name(uintptr_t object,std::string& out) {
    bool ok=true;auto vt=ptr(object+16,ok),traits=ptr(vt+40,ok),name=ptr(traits+144,ok);
    return ok && name && string(name,out);
}
bool location(uintptr_t object,float& x,float& y) {
    bool ok=true;auto loc=ptr(object+go::kMapEntity_position,ok);
    auto a=read<double>(loc+go::kPointLite_x,ok),b=read<double>(loc+go::kPointLite_y,ok);
    if(!ok || !std::isfinite(a)||!std::isfinite(b)||std::abs(a)>100000||std::abs(b)>100000)return false;
    x=static_cast<float>(a);y=static_cast<float>(b);return true;
}
bool bindable(uintptr_t owner,size_t offset,int32_t& value) {
    bool ok=true;auto p=ptr(owner+offset,ok);
    if(!ok||!p)return false;
    auto d=read<double>(p+go::kBindableInt_value,ok);
    if(!ok||!std::isfinite(d)||d<0||d>2147483647)return false;
    value=static_cast<int32_t>(d);return true;
}
uint64_t unix_ms() {FILETIME t;GetSystemTimeAsFileTime(&t);ULARGE_INTEGER u;u.LowPart=t.dwLowDateTime;u.HighPart=t.dwHighDateTime;return (u.QuadPart-116444736000000000ULL)/10000;}
}
struct NativeState::Impl {
    HANDLE mapping=nullptr;uint8_t* bytes=nullptr;
    NativeDiscovery mode;
    uintptr_t application=0,anchor=0,scan=0,pass_app=0;uint64_t retry_at=0,frame=0,revision=0;
    uint64_t nonce[2]{},parity_checks=0,parity_mismatches=0;std::string nonce_text;
    uint32_t cost=0;const char* failure="native roots not discovered";const char* root="none";
    std::vector<uint8_t> scan_buffer=std::vector<uint8_t>(1024*1024);
    std::vector<Entity> records;std::string names,last_navigation;
    Impl(NativeDiscovery discovery,bool named):mode(discovery) {
        // Out-of-process readers open the named mapping; an in-process host
        // hands the same frame to its caller through xf_live_frame.
        auto name=L"Local\\xenomit_state_"+std::to_wstring(GetCurrentProcessId());
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,MappingBytes,named?name.c_str():nullptr);
        if(mapping)bytes=static_cast<uint8_t*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,MappingBytes));
        records.reserve(Capacity);names.reserve(64*1024);
        // Integers in [2^52, 2^53): exact doubles, exact in decimal and as AS3
        // Numbers, never zero/denormal, and ~104 random bits per session.
        for(int i=0;i<2;++i) {
            unsigned lo=0,hi=0;rand_s(&lo);rand_s(&hi);
            auto value=(uint64_t(1)<<52)|((uint64_t(hi)<<32|lo)&((uint64_t(1)<<52)-1));
            auto d=static_cast<double>(value);std::memcpy(&nonce[i],&d,8);
            nonce_text+=(i?",":"")+std::to_string(value);
        }
    }
    ~Impl(){if(bytes)UnmapViewOfFile(bytes);if(mapping)CloseHandle(mapping);}
    // Class identity plus the generated method fingerprints of this build.
    uintptr_t validate_screen(uintptr_t s) {
        bool ok=true;std::string name;
        if(!s||!class_name(s,name)||name!="ScreenManager")return 0;
        auto event=ptr(s+go::kScreenManager_eventManager,ok);
        if(!ok||!class_name(event,name)||name!="EventManager")return 0;
        auto timer=ptr(ptr(s+16,ok)+0x78+go::kScreenManager_handleEnterFrame_DispId*8,ok);
        auto getter=ptr(ptr(event+16,ok)+0x78+go::kEventManager_hero_DispId*8,ok);
        auto move=ptr(ptr(event+16,ok)+0x78+go::kEventManager_moveHeroToCordinates_DispId*8,ok);
        auto ti=ptr(timer+16,ok),gi=ptr(getter+16,ok),mi=ptr(move+16,ok);
        if(!ok||read<int32_t>(ti+64,ok)!=go::kScreenManager_handleEnterFrame_MethodId||read<int32_t>(gi+64,ok)!=go::kEventManager_hero_MethodId||read<int32_t>(mi+64,ok)!=go::kEventManager_moveHeroToCordinates_MethodId||ptr(ti+48,ok)!=ptr(gi+48,ok)||ptr(gi+48,ok)!=ptr(mi+48,ok)||!ok)return 0;
        return s;
    }
    uintptr_t screen_from_app(uintptr_t app) {
        bool ok=true;auto main=ptr(app+go::kMainClientApplication_main,ok),s=ptr(main+go::kMain_screenManager,ok);
        return ok?validate_screen(s):0;
    }
    bool anchor_valid(uintptr_t a) {
        bool ok=true;std::string name;
        return class_name(a,name)&&name=="NativeAnchor"&&read<uint64_t>(a+AnchorTag0,ok)==nonce[0]&&read<uint64_t>(a+AnchorTag0+8,ok)==nonce[1]&&ok;
    }
    uintptr_t screen_from_anchor(uintptr_t a) {
        bool ok=true;auto s=ptr(a+AnchorScreen,ok); // Object slot: tagged atom, masked by ptr()
        return ok?validate_screen(s):0;
    }
    bool pattern_at(const uint8_t* p) {
        for(size_t k=0;k<sizeof(go::kAppPattern);++k)if((p[k]^go::kAppPattern[k])&go::kAppPatternMask[k])return false;
        return true;
    }
    bool wants_anchor() const {return mode!=NativeDiscovery::Pattern;}
    bool wants_pattern() const {return mode!=NativeDiscovery::Anchor;}
    bool have_roots() const {
        switch(mode) {
            case NativeDiscovery::Anchor: return anchor!=0;
            case NativeDiscovery::Pattern: return application!=0;
            case NativeDiscovery::Parity: return anchor&&application;
            default: return anchor||application;
        }
    }
    // Bounded discovery slices; no process-wide scan on a hot frame. One pass
    // looks for both roots; Auto takes the pattern only after a full pass
    // without the anchor.
    bool discover() {
        if(GetTickCount64()<retry_at)return false;
        for(int work=0;work<4;++work) {
            MEMORY_BASIC_INFORMATION mbi{};
            if(!VirtualQuery(reinterpret_cast<void*>(scan),&mbi,sizeof(mbi))) {
                scan=0;retry_at=GetTickCount64()+2000;
                if(mode==NativeDiscovery::Auto&&!anchor&&pass_app)application=pass_app;
                pass_app=0;return have_roots();
            }
            auto end=reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
            if(mbi.State!=MEM_COMMIT || (mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !(mbi.Protect&0xee)) {scan=end;continue;}
            auto amount=std::min(scan_buffer.size(),static_cast<size_t>(end-scan));
            if(copy(scan_buffer.data(),scan,amount)) {
                const auto* buf=scan_buffer.data();
                for(size_t i=0;i+16<=amount;i+=4) {
                    if(wants_anchor()&&!anchor&&!(i&7)&&std::memcmp(buf+i,&nonce[0],8)==0&&std::memcmp(buf+i+8,&nonce[1],8)==0) {
                        auto a=scan+i-AnchorTag0;
                        if(anchor_valid(a))anchor=a;
                    }
                    if(wants_pattern()&&!application&&!pass_app&&buf[i]==go::kAppPattern[0]&&i+sizeof(go::kAppPattern)<=amount&&pattern_at(buf+i)) {
                        auto app=scan+i-go::kAppPatternOffset;
                        if(screen_from_app(app))pass_app=app;
                    }
                }
            }
            if(pass_app&&mode!=NativeDiscovery::Auto){application=pass_app;pass_app=0;}
            scan+=amount;if(scan<end)scan-=sizeof(go::kAppPattern); // preserve boundary matches
            if(mode==NativeDiscovery::Auto?anchor!=0:have_roots()){scan=0;pass_app=0;return true;}
        }
        return false;
    }
    // ScreenManager for this frame, or 0. A valid anchor that merely lacks a
    // ScreenManager (login screen, relogin) fails the frame without a rescan.
    uintptr_t screen() {
        if(anchor&&!anchor_valid(anchor))anchor=0;
        if(application&&!screen_from_app(application))application=0;
        if(!have_roots()) {
            root="none";
            if(!discover()){failure="native roots not discovered";return 0;}
        }
        uintptr_t s=0;
        if(anchor){s=screen_from_anchor(anchor);root="anchor";if(!s){failure="native anchor has no valid ScreenManager yet";return 0;}}
        else {s=screen_from_app(application);root="pattern";if(!s){application=0;failure="native roots not discovered";return 0;}}
        if(mode==NativeDiscovery::Parity) {
            auto legacy=screen_from_app(application);++parity_checks;
            if(legacy!=s) {
                ++parity_mismatches;
                std::fprintf(stderr,"Native discovery parity mismatch: anchor=%p pattern=%p\n",reinterpret_cast<void*>(s),reinterpret_cast<void*>(legacy));
                failure="native discovery parity mismatch";return 0;
            }
        }
        return s;
    }
    bool capture(const std::string& nav,uint64_t at) {
        if(!bytes){failure="shared memory allocation failed";return false;}
        LARGE_INTEGER start,finish,freq;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&start);
        auto s=screen();
        if(!s)return false;
        bool ok=true;auto map=ptr(s+go::kScreenManager_map,ok),hero=ptr(s+go::kScreenManager_hero,ok);std::string cls;
        Header h{};h.magic=0x4d545358;h.version=2;h.map_id=read<int32_t>(map+go::kMap_id,ok);
        auto width=read<int32_t>(map+go::kMap_width,ok),height=read<int32_t>(map+go::kMap_height,ok);
        h.width=static_cast<float>(width);h.height=static_cast<float>(height);
        h.hero_id=read<int32_t>(hero+go::kMapEntity_id,ok);
        if(!ok||!class_name(map,cls)||cls!="Map"||!class_name(hero,cls)||cls!="Ship"||h.map_id<1||width<100||height<100||width>100000||height>100000||h.hero_id<=0||!location(hero,h.hero_x,h.hero_y)) {failure="native root validation failed";return false;}
        // Supplementary managers/NOA come from the existing slower bridge.
        // Never pair fresh positions with stale geometry or another map.
        const auto mapKey="\"map_id\":"+std::to_string(h.map_id);
        auto pos=nav.find(mapKey);
        if(!at||GetTickCount64()-at>750||pos==std::string::npos||(pos+mapKey.size()<nav.size()&&nav[pos+mapKey.size()]!=','&&nav[pos+mapKey.size()]!='}')) {failure="native navigation unavailable or stale";return false;}
        auto v=ptr(map+go::kMap_entities,ok),table=ptr(v+48,ok);auto count=read<uint32_t>(v+56,ok);
        if(!ok||count>Capacity){failure="native entity vector rejected";return false;}
        records.clear();names.clear();h.hero_flags=1;
        // Selected entity: Map._lockManager._target, set on click. A null lock
        // or unreadable chain reports "no target"; the flag marks it as valid.
        {
            bool tok=true;auto lock=ptr(map+go::kMap_lockManager,tok);
            auto sel=lock?ptr(lock+go::kLockManager_target,tok):0;
            int32_t tid=0;
            if(tok&&sel){bool iok=true;auto id=read<int32_t>(sel+go::kMapEntity_id,iok);if(iok&&id>0)tid=id;}
            if(tok){h.target_id=tid;h.hero_flags|=4;}
        }
        for(uint32_t i=0;i<count;++i) {
            auto e=ptr(table+16+i*8,ok);Entity r{};r.id=read<int32_t>(e+go::kMapEntity_id,ok);
            if(!ok||ptr(e+go::kMapEntity_container,ok)!=map||!location(e,r.x,r.y)||!class_name(e,cls)){failure="native entity validation failed";return false;}
            std::string name;
            bool ship=cls=="Ship"||cls=="Pet";
            if(ship) {
                auto npc=read<int32_t>(e+go::kShip_isNpc,ok);
                if(npc!=0&&npc!=1){failure="native ship layout rejected";return false;}
                r.kind=cls=="Pet"?4:npc?3:2;r.flags=npc?1:0;
                auto health=ptr(e+go::kShip_hpTrait,ok);
                if(health) {
                    if(!bindable(health,go::kHPTrait_hp,r.hp)||!bindable(health,go::kHPTrait_maxHp,r.max_hp)||!bindable(health,go::kHPTrait_shield,r.shield)||!bindable(health,go::kHPTrait_maxShield,r.max_shield)||!bindable(health,go::kHPTrait_nanohullHp,r.nano)||!bindable(health,go::kHPTrait_nanohullMaxHp,r.max_nano)){failure="native health layout rejected";return false;}
                    r.flags|=8;
                }
                auto player=ptr(e+go::kShip_info,ok);
                if(player) {
                    bindable(player,go::kInfoTrait_clanId,r.clan);bindable(player,go::kInfoTrait_clanDiplomacy,r.diplomacy);bindable(player,go::kInfoTrait_factionId,r.faction);
                    auto ns=ptr(player+go::kInfoTrait_username,ok);if(ns&&!string(ptr(ns+go::kBindableString_value,ok),name)){failure="native username rejected";return false;}
                }
                auto move=ptr(e+go::kShip_moveTrait,ok),dest=move?ptr(move+go::kMoveTrait_destination,ok):0;
                if(dest) {
                    auto dx=read<double>(dest+go::kPointLite_x,ok),dy=read<double>(dest+go::kPointLite_y,ok);
                    if(!std::isfinite(dx)||!std::isfinite(dy)||std::abs(dx)>100000||std::abs(dy)>100000){failure="native destination rejected";return false;}
                    r.dx=static_cast<float>(dx);r.dy=static_cast<float>(dy);
                    if(std::abs(r.dx-r.x)>1||std::abs(r.dy-r.y)>1)r.flags|=4;
                }
            } else if(cls=="Portal") {r.kind=7;r.flags|=16;r.type=read<int32_t>(e+go::kPortal_designId,ok);}
            else if(cls=="Box"||cls=="Ore") {r.kind=cls=="Box"?5:6;r.flags|=32;if(!string(ptr(e+go::kCollectable_hash,ok),name)){failure="native collectable hash rejected";return false;}}
            else if(cls=="Station")r.kind=9;
            else if(cls=="Headquarters"||cls=="Hangar"||cls=="RepairStation"||cls=="Refinery"||cls=="QuestGiver"||cls=="HomeBaseLaserTurret")r.kind=8;
            else if(cls=="Mine")r.kind=11;
            else if(cls=="Beacon")r.kind=10;
            if(!ok){failure="native pointer read failed";return false;}
            if(e==hero) {
                // Hot hero state, read natively on every frame: current attack
                // target (AttackerTrait in the traits vector) and move speed.
                bool hok=true;auto traits=ptr(hero+go::kEntityTraitContainer_traits,hok);
                auto ttable=traits?ptr(traits+48,hok):0;auto tcount=traits?read<uint32_t>(traits+56,hok):0;
                if(hok&&ttable&&tcount<=64) {
                    int32_t attack=0;std::string tn;
                    for(uint32_t t=0;t<tcount;++t) {
                        bool tok=true;auto trait=ptr(ttable+16+t*8,tok);
                        if(!tok||!trait||!class_name(trait,tn)||tn!="AttackerTrait")continue;
                        auto owner=ptr(trait+go::kAttackerTrait_targetOwner,tok);
                        if(tok&&owner){auto id=read<int32_t>(owner+go::kMapEntity_id,tok);if(tok&&id>0)attack=id;}
                        if(tok){h.attack_target_id=attack;h.hero_flags|=8;}
                        break;
                    }
                }
                bool sok=true;auto mt=ptr(hero+go::kShip_moveTrait,sok);int32_t sp=0;
                if(sok&&mt&&bindable(mt,go::kMoveTrait_speed,sp)){h.hero_speed=sp;h.hero_flags|=16;}
            }
            if(e==hero){r.kind=1;r.flags|=2;h.faction=r.faction;h.clan=r.clan;h.dest_x=r.dx;h.dest_y=r.dy;if(r.flags&4)h.hero_flags|=2;}
            r.name_off=static_cast<uint32_t>(names.size());r.name_len=static_cast<uint32_t>(name.size());names+=name;records.push_back(r);
        }
        if(std::none_of(records.begin(),records.end(),[](const Entity& r){return r.kind==1;})){failure="hero absent from native entity vector";return false;}
        h.navigation_off=static_cast<uint32_t>(names.size());h.navigation_len=static_cast<uint32_t>(nav.size());
        if(names.size()+nav.size()>MappingBytes-Strings){failure="native string table overflow";return false;}
        if(nav!=last_navigation){last_navigation=nav;++revision;}h.navigation_revision=revision;
        h.entities=static_cast<uint32_t>(records.size());h.strings=static_cast<uint32_t>(names.size()+nav.size());h.frame=++frame;h.timestamp=unix_ms();
        auto seq=reinterpret_cast<volatile LONG*>(bytes+8);auto next=InterlockedIncrement(seq);
        if(!(next&1))InterlockedIncrement(seq);
        std::memcpy(bytes+128,records.data(),records.size()*sizeof(Entity));
        std::memcpy(bytes+Strings,names.data(),names.size());std::memcpy(bytes+Strings+names.size(),nav.data(),nav.size());
        QueryPerformanceCounter(&finish);cost=static_cast<uint32_t>((finish.QuadPart-start.QuadPart)*1000000/freq.QuadPart);h.capture_us=cost;
        // Leave seq alone while replacing the remainder of the header.
        std::memcpy(bytes,&h,8);std::memcpy(bytes+12,reinterpret_cast<uint8_t*>(&h)+12,sizeof(h)-12);
        MemoryBarrier();InterlockedIncrement(seq);failure="";return true;
    }
};
NativeState::NativeState(NativeDiscovery discovery,bool named):impl_(std::make_unique<Impl>(discovery,named)){}
NativeState::~NativeState()=default;
void NativeState::invalidate(){
    if(!impl_->bytes)return;
    auto seq=reinterpret_cast<volatile LONG*>(impl_->bytes+8);
    auto next=InterlockedIncrement(seq);if(!(next&1))InterlockedIncrement(seq);
    reinterpret_cast<Header*>(impl_->bytes)->timestamp=0;
    MemoryBarrier();InterlockedIncrement(seq);
}
bool NativeState::capture(const std::string& n,uint64_t at){return impl_->capture(n,at);}
uint32_t NativeState::capture_us()const{return impl_->cost;}
const char* NativeState::error()const{return impl_->failure;}
const std::string& NativeState::anchor_nonce()const{return impl_->nonce_text;}
const char* NativeState::root()const{return impl_->root;}
uint64_t NativeState::parity_checks()const{return impl_->parity_checks;}
uint64_t NativeState::parity_mismatches()const{return impl_->parity_mismatches;}
const uint8_t* NativeState::frame(size_t& size)const{size=impl_->bytes?MappingBytes:0;return impl_->bytes;}

namespace xf {
bool guarded_copy(void* to,uintptr_t from,size_t size){return copy(to,from,size);}
// InProcessPlayer queryBytes ([B)J region filter: committed, private, exactly
// PAGE_READWRITE, at most 32 MiB. Returns the first max matches in address order.
size_t query_bytes(const uint8_t* pattern,const uint8_t* mask,size_t len,uint64_t* out,size_t max) {
    if(!pattern||!len||len>4096||!max)return 0;
    std::vector<uint8_t> buffer(1024*1024+len);size_t found=0;uintptr_t at=0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while(found<max&&VirtualQuery(reinterpret_cast<void*>(at),&mbi,sizeof(mbi))) {
        auto base=reinterpret_cast<uintptr_t>(mbi.BaseAddress),end=base+mbi.RegionSize;
        if(end<=at)break;
        if(mbi.State==MEM_COMMIT&&mbi.Type==MEM_PRIVATE&&mbi.Protect==PAGE_READWRITE&&mbi.RegionSize<=0x2000000) {
            for(uintptr_t chunk=base;chunk<end&&found<max;) {
                auto amount=std::min(static_cast<size_t>(end-chunk),buffer.size());
                if(amount<len||!copy(buffer.data(),chunk,amount))break;
                for(size_t i=0;i+len<=amount&&found<max;++i) {
                    size_t k=0;
                    for(;k<len;++k)if((buffer[i+k]^pattern[k])&(mask?mask[k]:0xff))break;
                    if(k==len)out[found++]=chunk+i;
                }
                if(chunk+amount>=end)break;
                chunk+=amount-(len-1);
            }
        }
        at=end;
    }
    return found;
}
}
