#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "native_state.h"
#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {
// Exact xenomit-game-api live/layout.rs version 2 ABI.
struct Header {
    uint32_t magic, version, seq, pad;
    uint64_t frame, timestamp;
    uint32_t capture_us; int32_t map_id;
    float width,height; int32_t hero_id;
    float hero_x,hero_y,dest_x,dest_y;
    uint32_t hero_flags; int32_t faction,clan;
    uint32_t entities,strings; uint64_t navigation_revision;
    uint32_t navigation_off,navigation_len; uint8_t reserved[24];
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
// DarkBot BotInstaller's MainClientApplication fingerprint. Validate class
// identities and each decoded root before using this version-specific chain.
constexpr uint8_t Pattern[]={
1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,
2,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,
0,0,0,0,0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,0,
0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
0,0,0,0,1,0,0,0,0,0,0,0};
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
    bool ok=true;auto loc=ptr(object+64,ok);
    auto a=read<double>(loc+32,ok),b=read<double>(loc+40,ok);
    if(!ok || !std::isfinite(a)||!std::isfinite(b)||std::abs(a)>100000||std::abs(b)>100000)return false;
    x=static_cast<float>(a);y=static_cast<float>(b);return true;
}
bool bindable(uintptr_t owner,size_t offset,int32_t& value) {
    bool ok=true;auto p=ptr(owner+offset,ok);
    if(!ok||!p)return false;
    auto d=read<double>(p+56,ok);
    if(!ok||!std::isfinite(d)||d<0||d>2147483647)return false;
    value=static_cast<int32_t>(d);return true;
}
uint64_t unix_ms() {FILETIME t;GetSystemTimeAsFileTime(&t);ULARGE_INTEGER u;u.LowPart=t.dwLowDateTime;u.HighPart=t.dwHighDateTime;return (u.QuadPart-116444736000000000ULL)/10000;}
}
struct NativeState::Impl {
    HANDLE mapping=nullptr;uint8_t* bytes=nullptr;
    uintptr_t application=0,scan=0;uint64_t retry_at=0,frame=0,revision=0;
    uint32_t cost=0;const char* failure="native roots not discovered";
    std::vector<uint8_t> scan_buffer=std::vector<uint8_t>(1024*1024);
    std::vector<Entity> records;std::string names,last_navigation;
    Impl() {
        auto name=L"Local\\xenomit_state_"+std::to_wstring(GetCurrentProcessId());
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,MappingBytes,name.c_str());
        if(mapping)bytes=static_cast<uint8_t*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,MappingBytes));
        records.reserve(Capacity);names.reserve(64*1024);
    }
    ~Impl(){if(bytes)UnmapViewOfFile(bytes);if(mapping)CloseHandle(mapping);}
    uintptr_t screen(uintptr_t app) {
        bool ok=true;auto main=ptr(app+1344,ok),s=ptr(main+504,ok);std::string name;
        if(!ok||!class_name(s,name)||name!="ScreenManager")return 0;
        auto event=ptr(s+200,ok);
        if(!ok||!class_name(event,name)||name!="EventManager")return 0;
        auto timer=ptr(ptr(s+16,ok)+0x78+34*8,ok);
        auto getter=ptr(ptr(event+16,ok)+0x78+7*8,ok);
        auto move=ptr(ptr(event+16,ok)+0x78+10*8,ok);
        auto ti=ptr(timer+16,ok),gi=ptr(getter+16,ok),mi=ptr(move+16,ok);
        if(!ok||read<int32_t>(ti+64,ok)!=9581||read<int32_t>(gi+64,ok)!=17459||read<int32_t>(mi+64,ok)!=17461||ptr(ti+48,ok)!=ptr(gi+48,ok)||ptr(gi+48,ok)!=ptr(mi+48,ok)||!ok)return 0;
        return s;
    }
    // Bounded discovery slices; no process-wide scan on a hot frame.
    uintptr_t discover() {
        if(GetTickCount64()<retry_at)return 0;
        for(int work=0;work<4;++work) {
            MEMORY_BASIC_INFORMATION mbi{};
            if(!VirtualQuery(reinterpret_cast<void*>(scan),&mbi,sizeof(mbi))) {scan=0;retry_at=GetTickCount64()+2000;return 0;}
            auto end=reinterpret_cast<uintptr_t>(mbi.BaseAddress)+mbi.RegionSize;
            if(mbi.State!=MEM_COMMIT || (mbi.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !(mbi.Protect&0xee)) {scan=end;continue;}
            auto amount=std::min(scan_buffer.size(),static_cast<size_t>(end-scan));
            if(copy(scan_buffer.data(),scan,amount)) {
                for(size_t i=0;i+sizeof(Pattern)<=amount;i+=4) {
                    if(scan_buffer[i]!=1||std::memcmp(scan_buffer.data()+i,Pattern,sizeof(Pattern)))continue;
                    auto app=scan+i-228;auto s=screen(app);
                    if(s) {application=app;return s;}
                }
            }
            scan+=amount;if(scan<end)scan-=sizeof(Pattern); // preserve boundary matches
        }
        return 0;
    }
    bool capture(const std::string& nav,uint64_t at) {
        if(!bytes){failure="shared memory allocation failed";return false;}
        LARGE_INTEGER start,finish,freq;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&start);
        auto s=application?screen(application):discover();
        if(!s){application=0;failure="native roots not discovered";return false;}
        bool ok=true;auto map=ptr(s+256,ok),hero=ptr(s+240,ok);std::string cls;
        Header h{};h.magic=0x4d545358;h.version=2;h.map_id=read<int32_t>(map+84,ok);
        auto width=read<int32_t>(map+76,ok),height=read<int32_t>(map+80,ok);
        h.width=static_cast<float>(width);h.height=static_cast<float>(height);
        h.hero_id=read<int32_t>(hero+56,ok);
        if(!ok||!class_name(map,cls)||cls!="Map"||!class_name(hero,cls)||cls!="Ship"||h.map_id<1||width<100||height<100||width>100000||height>100000||h.hero_id<=0||!location(hero,h.hero_x,h.hero_y)) {failure="native root validation failed";return false;}
        // Supplementary managers/NOA come from the existing slower bridge.
        // Never pair fresh positions with stale geometry or another map.
        const auto mapKey="\"map_id\":"+std::to_string(h.map_id);
        auto pos=nav.find(mapKey);
        if(!at||GetTickCount64()-at>750||pos==std::string::npos||(pos+mapKey.size()<nav.size()&&nav[pos+mapKey.size()]!=','&&nav[pos+mapKey.size()]!='}')) {failure="native navigation unavailable or stale";return false;}
        auto v=ptr(map+40,ok),table=ptr(v+48,ok);auto count=read<uint32_t>(v+56,ok);
        if(!ok||count>Capacity){failure="native entity vector rejected";return false;}
        records.clear();names.clear();h.hero_flags=1;
        for(uint32_t i=0;i<count;++i) {
            auto e=ptr(table+16+i*8,ok);Entity r{};r.id=read<int32_t>(e+56,ok);
            if(!ok||ptr(e+96,ok)!=map||!location(e,r.x,r.y)||!class_name(e,cls)){failure="native entity validation failed";return false;}
            std::string name;
            bool ship=cls=="Ship"||cls=="Pet";
            if(ship) {
                auto npc=read<int32_t>(e+112,ok);
                if(npc!=0&&npc!=1){failure="native ship layout rejected";return false;}
                r.kind=cls=="Pet"?4:npc?3:2;r.flags=npc?1:0;
                auto health=ptr(e+184,ok);
                if(health) {
                    if(!bindable(health,48,r.hp)||!bindable(health,56,r.max_hp)||!bindable(health,64,r.shield)||!bindable(health,72,r.max_shield)||!bindable(health,80,r.nano)||!bindable(health,88,r.max_nano)){failure="native health layout rejected";return false;}
                    r.flags|=8;
                }
                auto player=ptr(e+248,ok);
                if(player) {
                    bindable(player,40,r.clan);bindable(player,48,r.diplomacy);bindable(player,72,r.faction);
                    auto ns=ptr(player+64,ok);if(ns&&!string(ptr(ns+40,ok),name)){failure="native username rejected";return false;}
                }
                auto move=ptr(e+232,ok),dest=move?ptr(move+104,ok):0;
                if(dest) {
                    auto dx=read<double>(dest+32,ok),dy=read<double>(dest+40,ok);
                    if(!std::isfinite(dx)||!std::isfinite(dy)||std::abs(dx)>100000||std::abs(dy)>100000){failure="native destination rejected";return false;}
                    r.dx=static_cast<float>(dx);r.dy=static_cast<float>(dy);
                    if(std::abs(r.dx-r.x)>1||std::abs(r.dy-r.y)>1)r.flags|=4;
                }
            } else if(cls=="Portal") {r.kind=7;r.flags|=16;r.type=read<int32_t>(e+120,ok);}
            else if(cls=="Box"||cls=="Ore") {r.kind=cls=="Box"?5:6;r.flags|=32;if(!string(ptr(e+160,ok),name)){failure="native collectable hash rejected";return false;}}
            else if(cls=="Station")r.kind=9;
            else if(cls=="Headquarters"||cls=="Hangar"||cls=="RepairStation"||cls=="Refinery"||cls=="QuestGiver"||cls=="HomeBaseLaserTurret")r.kind=8;
            else if(cls=="Mine")r.kind=11;
            else if(cls=="Beacon")r.kind=10;
            if(!ok){failure="native pointer read failed";return false;}
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
NativeState::NativeState():impl_(std::make_unique<Impl>()){}
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
