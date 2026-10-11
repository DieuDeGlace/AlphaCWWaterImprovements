#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include "ReedShore.h"
#include "ReedOwnership.h"

using Bytes=std::vector<unsigned char>;
static HMODULE ownModule;
void InitializeAlphaReeds(HMODULE self);
extern "C" int __cdecl ReedSelfTest();
static std::wstring dllPath(){wchar_t b[32768];GetModuleFileNameW(ownModule,b,32768);return b;}
static std::wstring parent(std::wstring p){return p.substr(0,p.find_last_of(L"\\/"));}
static std::wstring root(){return parent(parent(dllPath()));}
static void log(const char* text){FILE* f=nullptr;_wfopen_s(&f,(root()+L"\\water-port\\native-water.log").c_str(),L"a");if(f){fprintf(f,"%llu %s\n",GetTickCount64(),text);fclose(f);}}
static std::string digest(const std::wstring& path){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return {};
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;std::string result;
    if(CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)){
        unsigned char buffer[65536];DWORD got=0;bool ok=true;
        while(ReadFile(file,buffer,sizeof(buffer),&got,nullptr)&&got){if(!CryptHashData(hash,buffer,got,0)){ok=false;break;}}
        unsigned char value[32];DWORD size=32;
        if(ok&&CryptGetHashParam(hash,HP_HASHVAL,value,&size,0)){char hex[3];for(auto b:value){sprintf_s(hex,"%02x",b);result+=hex;}}
    }
    if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);CloseHandle(file);return result;
}
static bool plainWater(const Bytes& b){if(b.empty()||b.size()%4|| (b[3]&31)!=3)return false;for(size_t i=7;i<b.size();i+=4)if(b[i]&31)return false;return true;}
static Bytes deepen(const Bytes& b,int depth){
    if(!plainWater(b))return {};int count=(int)b.size()/4;depth=std::min(depth,count);Bytes out=b;
    for(int i=0;i<count;++i){if(i<depth){memcpy(out.data()+i*4,b.data(),4);out[i*4+3]=(b[3]&224)|2;}else memcpy(out.data()+i*4,b.data()+4,4);}
    return out;
}
static Bytes liquidize(Bytes b){for(size_t i=3;i<b.size();i+=4)if((b[i]&31)==3)b[i]=(b[i]&224)|2;return b;}
template<class T>static T value(const Bytes& b,size_t offset){T v{};if(offset+sizeof(T)<=b.size())memcpy(&v,b.data()+offset,sizeof(T));return v;}
struct Change{int x,y,surface,oldBase,newBase;DWORD column,data,count;Bytes before,after;std::vector<DWORD> flags;};
struct Patch{
    HANDLE process{};DWORD pid{},base{},dataRva{},dataSize{},vtable{},game{},layout{};ULONGLONG lastFind{},lastReeds{},lastReedLog{};
    std::map<std::pair<int,int>,int> converted;
    using NtCall=LONG(NTAPI*)(HANDLE);NtCall suspend{},resume{};
    ~Patch(){if(process)CloseHandle(process);}
    Bytes read(DWORD address,size_t count){
        if(!address||count>16*1024*1024)return {};Bytes b(count);SIZE_T got=0;
        if(!ReadProcessMemory(process,(LPCVOID)(uintptr_t)address,b.data(),count,&got)||got!=count)return {};return b;
    }
    DWORD ptr(DWORD a){return value<DWORD>(read(a,4),0);}
    bool write(DWORD address,const Bytes& b){SIZE_T got=0;return WriteProcessMemory(process,(LPVOID)(uintptr_t)address,b.data(),b.size(),&got)&&got==b.size()&&read(address,b.size())==b;}
    Bytes intBytes(int v){Bytes b(4);memcpy(b.data(),&v,4);return b;}
    bool init(DWORD id){
        pid=id;HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);MODULEENTRY32W m{};m.dwSize=sizeof(m);
        bool ok=Module32FirstW(snap,&m)!=0;CloseHandle(snap);if(!ok)return false;
        if(_wcsicmp(parent(m.szExePath).c_str(),root().c_str()))return false;
        std::string sha=digest(m.szExePath);
        if(sha=="84a7a132a84d4282338e7ea45a1940d32066d64e39cbafed8f2418d5a6dc30bf"){vtable=0x2fd8cc;layout=8;}
        else if(sha=="a4eeb3606ad2b82e4c9b3d0db6f9472ffa6e89a4d56084a9114ff1fb3c812699"||sha=="88798362b3516e339c4ee5f6defad2a8c36a5f0930a51e6a3e497b9531e382c6")vtable=0x2fc8cc;
        else{log("Unsupported executable: no terrain changes made");return false;}
        base=(DWORD)(uintptr_t)m.modBaseAddr;process=OpenProcess(0xC38,FALSE,pid);if(!process)return false;
        auto ntdll=GetModuleHandleW(L"ntdll.dll");suspend=(NtCall)GetProcAddress(ntdll,"NtSuspendProcess");resume=(NtCall)GetProcAddress(ntdll,"NtResumeProcess");if(!suspend||!resume)return false;
        Bytes header=read(base,4096);DWORD pe=value<DWORD>(header,0x3c);WORD sections=value<WORD>(header,pe+6),optional=value<WORD>(header,pe+20);
        for(int i=0;i<sections;++i){size_t o=pe+24+optional+i*40;if(o+40>header.size())return false;if(!memcmp(header.data()+o,".data\0\0\0",8)){dataSize=value<DWORD>(header,o+8);dataRva=value<DWORD>(header,o+12);}}
        return dataRva&&dataSize&&dataSize<16*1024*1024;
    }
    bool player(DWORD g,int& x,int& y,int& z){
        DWORD p=ptr(g+layout+0x8006c8);Bytes b=read(p,0x150);if(b.empty()||value<DWORD>(b,0)!=base+vtable)return false;
        for(int i=0;i<3;++i){float dim=value<float>(b,0x80+i*4);if(!std::isfinite(dim)||dim<=.1||dim>=20)return false;}
        double xx=value<int64_t>(b,16)/65536.0,yy=value<int64_t>(b,24)/65536.0,zz=value<int64_t>(b,32)/65536.0;
        if(!(xx>0&&xx<16777216&&yy>0&&yy<16777216&&zz>-10000&&zz<10000))return false;
        x=(int)std::floor(xx);y=(int)std::floor(yy);z=(int)std::floor(zz);return true;
    }
    void find(){
        Bytes b=read(base+dataRva,dataSize);std::set<DWORD> candidates;
        auto add=[&](const Bytes& block){for(size_t i=0;i+4<=block.size();i+=4){DWORD p=value<DWORD>(block,i);if(p>0x10000&&p<0x7fff0000&&p%4==0)candidates.insert(p);}};
        add(b);auto first=candidates;for(DWORD p:first)add(read(p,512));
        DWORD found=0;int matches=0,x,y,z;for(DWORD p:candidates)if(player(p,x,y,z)){found=p;++matches;}
        if(matches==1){game=found;log("Located active character");}
    }
    DWORD columns(int x,int y){
        if(x<0||y<0||x>=16777216||y>=16777216)return 0;int zx=x/256,zy=y/256;
        DWORD region=ptr(game+layout+0x398+((zx/64)*1024+zy/64)*4);DWORD zone=region?ptr(region+0x10018+((zx%64)*64+zy%64)*4):0;
        Bytes xy=read(zone+0x60,8);if(!zone||xy.size()!=8||value<int>(xy,0)!=zx||value<int>(xy,4)!=zy)return 0;return ptr(zone+0xa8);
    }
    std::vector<DWORD> flags(int x,int y){
        DWORD width=ptr(game+layout+0x2d4),cache=ptr(game+layout+0x2d8);std::vector<DWORD> out;if(!cache||width<1||width>64)return out;
        for(int cx=(x-1)/32;cx<=(x+1)/32;++cx)for(int cy=(y-1)/32;cy<=(y+1)/32;++cy){DWORD chunk=cache+((cy%width)*width+cx%width)*0x268;Bytes xy=read(chunk+0x18,8);if(xy.size()==8&&value<int>(xy,0)==cx&&value<int>(xy,4)==cy)out.push_back(chunk+0x74);}return out;
    }
    bool surface(int x,int y,int& height){
        DWORD c=columns(x,y);if(!c)return false;Bytes col=read(c+(x%256+y%256*256)*32,32);DWORD count=value<DWORD>(col,28);if(count<1||count>2048)return false;
        Bytes cells=read(value<DWORD>(col,24),count*4);if(cells.size()!=count*4)return false;int bz=value<int>(col,16);
        if(plainWater(cells)){height=bz;return true;}int top=-1;for(DWORD i=0;i<count;++i){int t=cells[i*4+3]&31;if(t!=0&&t!=2)return false;if(t==2)top=i;}
        if(top<0)return false;height=bz+top;return true;
    }
    int depth(int x,int y,int height){int h;for(auto d:std::array<std::pair<int,int>,4>{{{-1,0},{1,0},{0,-1},{0,1}}})if(surface(x+d.first,y+d.second,h)&&h!=height)return 2;return 3;}
    bool journal(const std::vector<Change>& batch){
        FILE* f=nullptr;_wfopen_s(&f,(root()+L"\\water-port\\native-water-originals.bin").c_str(),L"ab");if(!f)return false;
        DWORD header[]={0x41575031,pid,(DWORD)batch.size()};bool ok=fwrite(header,sizeof(header),1,f)==1;
        for(const auto& c:batch){DWORD row[]={(DWORD)c.x,(DWORD)c.y,c.column,c.data,c.count,(DWORD)c.oldBase,(DWORD)c.newBase};ok=ok&&fwrite(row,sizeof(row),1,f)==1;ok=ok&&fwrite(c.before.data(),c.before.size(),1,f)==1;}
        ok=ok&&fflush(f)==0;fclose(f);return ok;
    }
    ShoreCell shoreCell(int x,int y){ShoreCell out;DWORD cols=columns(x,y);if(!cols)return out;Bytes column=read(cols+(x%256+y%256*256)*32,32);DWORD count=value<DWORD>(column,28);int bz=value<int>(column,16);if(count<1||count>2048)return out;Bytes cells=read(value<DWORD>(column,24),count*4);if(cells.size()!=count*4)return out;out.valid=true;int highestWater=-1;for(DWORD i=0;i<count;i++){int type=cells[i*4+3]&31;if(type==2||type==3){highestWater=(int)i;out.water=bz+i;}else if(type){out.ground=bz+i;out.groundType=type;}}if(highestWater>=0){for(int i=highestWater;i>=0;i--){int type=cells[i*4+3]&31;if(type!=2&&type!=3)break;out.depth++;}if(out.ground>=out.water){out.depth=0;out.water=-10000;}}return out;}
    void repairReedOwners(){
        int px,py,pz;if(!game||!player(game,px,py,pz))return;DWORD width=ptr(game+layout+0x2d4),cache=ptr(game+layout+0x2d8);if(!cache||width<1||width>64)return;
        auto chunkAt=[&](int cx,int cy){DWORD c=cache+((cy%width)*width+cx%width)*0x268;Bytes xy=read(c+0x18,8);return xy.size()==8&&value<int>(xy,0)==cx&&value<int>(xy,4)==cy?c:0;};
        for(int cy=(py-96)/32;cy<=(py+96)/32;cy++)for(int cx=(px-96)/32;cx<=(px+96)/32;cx++){
            DWORD source=chunkAt(cx,cy);if(!source)continue;DWORD head=ptr(source+0x248),node=ptr(head);std::set<DWORD> seen;
            while(node&&node!=head&&seen.size()<4096){if(!seen.insert(node).second)break;Bytes record=read(node,72);if(record.size()!=72)break;DWORD next=value<DWORD>(record,0);
                int x=(int)(value<int64_t>(record,16)/65536),y=(int)(value<int64_t>(record,24)/65536);
                if(value<DWORD>(record,8)!=22||x/32==cx&&y/32==cy){node=next;continue;}DWORD destination=chunkAt(x/32,y/32);if(!destination){node=next;continue;}
                if(suspend(process)!=0)return;std::map<DWORD,std::pair<Bytes,Bytes>> edits;size_t applied=0;
                try{
                    if(read(node,72)!=record||ptr(source+0x248)!=head||!destination)throw std::runtime_error("Reed ownership changed before repair");
                    DWORD nodes[2]{};
                    for(int copy=0;copy<2;copy++){
                        DWORD h=ptr(source+0x240+copy*8),n=ptr(h);unsigned count=0,matches=0;std::set<DWORD> visited;
                        while(n&&n!=h&&visited.size()<4096){if(!visited.insert(n).second)break;Bytes b=read(n,72);if(b.size()!=72)break;if(value<DWORD>(b,8)==22&&!memcmp(b.data()+16,record.data()+16,24)){nodes[copy]=n;matches++;}n=value<DWORD>(b,0);count++;}
                        if(n!=h||matches!=1||count!=ptr(source+0x244+copy*8))throw std::runtime_error("Native foliage list incomplete");
                    }
                    auto stage=[&](DWORD address,const Bytes& after){auto old=read(address,after.size());if(old.size()!=after.size())throw std::runtime_error("Ownership read failed");auto it=edits.find(address);if(it==edits.end())edits.emplace(address,std::make_pair(old,after));else it->second.second=after;};
                    for(int copy=0;copy<2;copy++){
                        DWORD sh=ptr(source+0x240+copy*8),dh=ptr(destination+0x240+copy*8); transferReedLinks(nodes[copy],sh,dh,source+0x244+copy*8,destination+0x244+copy*8,[&](DWORD a){return ptr(a);},[&](DWORD a,DWORD v){stage(a,intBytes(v));});
                    }
                    // Preserve native vegetation: full terrain rebuild regenerates its original placements.
                    FILE* journal=nullptr;if(_wfopen_s(&journal,(root()+L"\\water-port\\reed-ownership-originals.bin").c_str(),L"ab")||!journal)throw std::runtime_error("Ownership journal unavailable");
                    bool ok=true;DWORD header[]={0x41524f31,pid,(DWORD)edits.size()};ok=fwrite(header,sizeof(header),1,journal)==1;
                    for(auto& e:edits){DWORD info[]={e.first,(DWORD)e.second.first.size()};ok=ok&&fwrite(info,sizeof(info),1,journal)==1&&fwrite(e.second.first.data(),e.second.first.size(),1,journal)==1&&fwrite(e.second.second.data(),e.second.second.size(),1,journal)==1;}
                    ok=ok&&fflush(journal)==0;fclose(journal);if(!ok)throw std::runtime_error("Ownership journal write failed");
                    for(auto& e:edits){++applied;if(!write(e.first,e.second.second))throw std::runtime_error("Ownership write failed");}
                    resume(process);log("Reed ownership transferred to bank chunk; native vegetation retained without terrain regeneration");return;
                }catch(const std::exception& e){size_t index=0;for(auto& edit:edits){if(index++>=applied)break;write(edit.first,edit.second.first);}resume(process);log(e.what());return;}
            }
        }
    }
    void relocateReeds(){
        ULONGLONG now=GetTickCount64();if(now-lastReeds<3000)return;lastReeds=now;int px,py,pz;if(!game||!player(game,px,py,pz))return;DWORD width=ptr(game+layout+0x2d4),cache=ptr(game+layout+0x2d8);if(!cache||width<1||width>64)return;
        struct Plant{DWORD node,head,chunk;Bytes before;std::array<int64_t,3> position;};struct Move{Plant first,second;Bytes destination;};struct Group{int cx,cy;std::map<std::array<int64_t,3>,std::vector<Plant>> first,second;};std::vector<Group> groups;unsigned examined=0;std::vector<Move> moves;std::map<std::pair<int,int>,ShoreCell> cells;std::set<std::pair<int,int>> reserved,rockLocations,reedLocations;
        auto cell=[&](int x,int y){auto key=std::make_pair(x,y);auto found=cells.find(key);if(found==cells.end())found=cells.emplace(key,shoreCell(x,y)).first;found->second.rock=rockLocations.count(key)!=0;return found->second;};
        for(int cy=(py-96)/32;cy<=(py+96)/32&&moves.size()<8;cy++)for(int cx=(px-96)/32;cx<=(px+96)/32&&moves.size()<8;cx++){
            DWORD chunk=cache+((cy%width)*width+cx%width)*0x268;Bytes xy=read(chunk+0x18,8);if(xy.size()!=8||value<int>(xy,0)!=cx||value<int>(xy,4)!=cy)continue;std::map<std::array<int64_t,3>,std::vector<Plant>> lists[2];bool complete=true;
            for(int copy=0;copy<2;copy++){DWORD head=ptr(chunk+0x240+copy*8),node=ptr(head);std::set<DWORD> seen;while(node&&node!=head&&seen.size()<4096){if(!seen.insert(node).second){complete=false;break;}Bytes b=read(node,72);if(b.size()!=72){complete=false;break;}DWORD kind=value<DWORD>(b,8);std::array<int64_t,3> position{value<int64_t>(b,16),value<int64_t>(b,24),value<int64_t>(b,32)};int x=(int)(position[0]/65536),y=(int)(position[1]/65536);{if(kind==22)lists[copy][position].push_back({node,head,chunk,b,position});if(kind==1){for(int ry=-4;ry<=4;ry++)for(int rx=-4;rx<=4;rx++)rockLocations.insert({x+rx,y+ry});}if(kind==22)reedLocations.insert({x,y});if(kind==1||kind==22||kind>=31)reserved.insert({x,y});}node=value<DWORD>(b,0);}if(node!=head)complete=false;}
            if(!complete)continue;groups.push_back({cx,cy,std::move(lists[0]),std::move(lists[1])});
        }
        for(auto& group:groups){int cx=group.cx,cy=group.cy;for(auto& entry:group.first){if(moves.size()>=8)break;auto second=group.second.find(entry.first);if(entry.second.size()!=1||second==group.second.end()||second->second.size()!=1)continue;auto& first=entry.second[0];auto& other=second->second[0];int x=(int)(entry.first[0]/65536),y=(int)(entry.first[1]/65536);if(std::abs(x-px)>96||std::abs(y-py)>96)continue;++examined;int nx,ny,nz;auto occupied=[&](int x,int y){if(reserved.count({x,y}))return true;for(auto& reed:reedLocations){int dx=x-reed.first,dy=y-reed.second;if(dx*dx+dy*dy<1.1f*1.1f)return true;}return false;};auto source=cell(x,y);if(source.depth==0&&dryBank(source))continue;auto placementCell=[&](int xx,int yy){auto c=cell(xx,yy);if(xx==x&&yy==y&&c.depth==0){c.depth=1;c.water=(int)(entry.first[2]/65536)-1;}return c;};if(!chooseReedBank(x,y,cx,cy,placementCell,occupied,nx,ny,nz))continue;Bytes destination(24);int64_t coords[3]={(int64_t)nx*65536+32768,(int64_t)ny*65536+32768,(int64_t)nz*65536};memcpy(destination.data(),coords,24);moves.push_back({first,other,destination});reserved.insert({nx,ny});reedLocations.insert({nx,ny});}
        }
        if(moves.empty()){if(examined&&now-lastReedLog>=15000){char message[128];sprintf_s(message,"Shore scan: %u paired clumps, no unoccupied dry bank within shoreline patches",examined);log(message);lastReedLog=now;}return;}if(suspend(process)!=0)throw std::runtime_error("Could not pause reed placements safely");size_t applied=0;FILE* journal=nullptr;
        try{
            std::vector<Move> valid;for(auto& move:moves){auto matches=[&](const Plant& p){return read(p.node,72)==p.before&&ptr(p.chunk+0x240+(p.head==ptr(p.chunk+0x248)?8:0))==p.head&&ptr(value<DWORD>(p.before,0)+4)==p.node&&ptr(value<DWORD>(p.before,4))==p.node;};if(matches(move.first)&&matches(move.second))valid.push_back(move);}
            if(!_wfopen_s(&journal,(root()+L"\\water-port\\reed-placement-originals.bin").c_str(),L"ab")&&journal){DWORD header[]={0x41525031,pid,(DWORD)valid.size()};if(fwrite(header,sizeof(header),1,journal)!=1)throw std::runtime_error("Reed journal failed");for(auto& move:valid){DWORD addresses[]={move.first.node,move.second.node};if(fwrite(addresses,sizeof(addresses),1,journal)!=1||fwrite(move.first.before.data()+16,24,1,journal)!=1||fwrite(move.destination.data(),24,1,journal)!=1)throw std::runtime_error("Reed journal failed");}if(fflush(journal))throw std::runtime_error("Reed journal flush failed");fclose(journal);journal=nullptr;}else throw std::runtime_error("Could not journal reed placements");
            moves=valid;for(auto& move:moves){++applied;if(!write(move.first.node+16,move.destination)||!write(move.second.node+16,move.destination))throw std::runtime_error("Reed placement readback failed");}
        }catch(...){if(journal)fclose(journal);for(size_t i=applied;i>0;i--){Bytes position(moves[i-1].first.before.begin()+16,moves[i-1].first.before.begin()+40);write(moves[i-1].first.node+16,position);write(moves[i-1].second.node+16,position);}resume(process);throw;}
        resume(process);if(applied){char message[128];sprintf_s(message,"Moved %u reed clumps ashore; native stems only, terrain/rocks unchanged",(unsigned)applied);log(message);}
    }
    int step(){
        int x,y,z;if(!game||!player(game,x,y,z)){ULONGLONG now=GetTickCount64();if(now-lastFind<3000)return 0;lastFind=now;find();if(!game||!player(game,x,y,z))return 0;}
        std::vector<Change> changes;
        for(int zx=std::max(0,x-64)/256;zx<=(x+64)/256&&changes.size()<128;++zx)for(int zy=std::max(0,y-64)/256;zy<=(y+64)/256&&changes.size()<128;++zy){
            DWORD cols=columns(zx*256,zy*256);if(!cols)continue;Bytes block=read(cols,256*256*32);if(block.empty())continue;
            for(int ny=std::max(y-64,zy*256);ny<=std::min(y+64,zy*256+255)&&changes.size()<128;++ny)for(int nx=std::max(x-64,zx*256);nx<=std::min(x+64,zx*256+255)&&changes.size()<128;++nx){
                size_t offset=(nx%256+ny%256*256)*32;DWORD count=value<DWORD>(block,offset+28),data=value<DWORD>(block,offset+24);int bz=value<int>(block,offset+16);
                if(count<1||count>2048||bz<=-10000||bz>=10000)continue;Bytes cells=read(data,count*4);if(cells.size()!=count*4)continue;
                bool hasSolid=false;int highest=-1;for(DWORD i=0;i<count;++i)if((cells[i*4+3]&31)==3){hasSolid=true;highest=i;}
                bool plain=plainWater(cells);int height=bz,newBase=bz;Bytes next;
                if(hasSolid&&!plain){next=liquidize(cells);height=bz+highest;}
                else{
                    Bytes original=cells;
                    if(!plain){auto known=converted.find({nx,ny});if(known==converted.end())continue;height=known->second;
                        int wet=0;while(wet<(int)count&&(cells[wet*4+3]&31)==2)++wet;bool valid=wet>0&&bz+wet-1==height;
                        for(int i=wet;i<(int)count;++i)if(cells[i*4+3]&31)valid=false;if(!valid)continue;
                        std::array<unsigned char,4> air{{255,255,255,0}};if(wet<(int)count)memcpy(air.data(),cells.data()+wet*4,4);
                        original[3]=(cells[3]&224)|3;for(DWORD i=1;i<count;++i)memcpy(original.data()+i*4,air.data(),4);
                    }
                    int d=std::min(depth(nx,ny,height),(int)count);next=deepen(original,d);newBase=height-(d-1);
                }
                if(next.empty()||(next==cells&&newBase==bz))continue;auto dirty=flags(nx,ny);if(dirty.empty())continue;
                changes.push_back({nx,ny,height,bz,newBase,cols+(DWORD)offset,data,count,cells,next,dirty});
            }
        }
        if(changes.empty())return 0;if(suspend(process)!=0)throw std::runtime_error("Could not pause terrain safely");
        std::vector<Change> valid;size_t done=0;
        try{
            for(auto& c:changes)if(ptr(c.column+24)==c.data&&ptr(c.column+28)==c.count&&read(c.column+16,4)==intBytes(c.oldBase)&&read(c.data,c.count*4)==c.before&&flags(c.x,c.y)==c.flags)valid.push_back(c);
            if(!valid.empty()&&!journal(valid))throw std::runtime_error("Could not journal terrain originals");
            for(auto& c:valid){++done;if(!write(c.data,c.after)||!write(c.column+16,intBytes(c.newBase)))throw std::runtime_error("Terrain write/readback failed");}
            for(auto& c:valid)for(DWORD f:c.flags)if(!write(f,Bytes{1}))throw std::runtime_error("Mesh update failed");
        }catch(...){for(size_t i=done;i>0;--i){auto& c=valid[i-1];write(c.data,c.before);write(c.column+16,intBytes(c.oldBase));}resume(process);throw;}
        resume(process);for(auto& c:valid)converted[{c.x,c.y}]=c.surface;
        if(!valid.empty()){char b[100];sprintf_s(b,"Converted %u columns; stones preserved",(unsigned)valid.size());log(b);}return (int)valid.size();
    }
};
extern "C" __declspec(dllexport) int __cdecl WaterSelfTest(){
    Bytes simple={20,40,60,0xa3,255,255,255,0,255,255,255,0};auto d=deepen(simple,3);
    if(d.size()!=12||d[3]!=0xa2||d[7]!=0xa2||d[11]!=0xa2)return 1;
    auto slope=deepen(simple,2);if(slope[11]!=0||slope[7]!=0xa2)return 2;
    Bytes single={1,2,3,3};if(deepen(single,3)!=Bytes({1,2,3,2}))return 3;
    Bytes mixed={1,2,3,3,7,8,9,1,4,5,6,0xe3};auto m=liquidize(mixed);
    if(m[3]!=2||m[11]!=0xe2||memcmp(m.data()+4,mixed.data()+4,4)||!deepen(mixed,3).empty())return 4;
    return ReedSelfTest()*100;
}
extern "C" __declspec(dllexport) void CALLBACK WaterWorker(HWND,HINSTANCE,LPSTR arguments,int){
    DWORD pid=strtoul(arguments,nullptr,10);if(!pid)return;
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\CubeAlphaAutomaticWater");if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(mutex);return;}
    try{Patch patch;if(!patch.init(pid)){log("Water worker could not attach to supported build");CloseHandle(mutex);return;}
        log("Native water + shore reeds worker active: 2-3 depth, stones preserved, no added root geometry");
        DWORD code=0;while(GetExitCodeProcess(patch.process,&code)&&code==STILL_ACTIVE){patch.step();patch.relocateReeds();patch.repairReedOwners();Sleep(500);}log("Water worker stopped with game");
    }catch(const std::exception& e){log(e.what());}catch(...){log("Water worker stopped after unexpected failure");}CloseHandle(mutex);
}
static DWORD WINAPI startWorker(LPVOID){
    InitializeAlphaReeds(ownModule);
    wchar_t system[32768];GetSystemDirectoryW(system,32768);std::wstring exe=std::wstring(system)+L"\\rundll32.exe";
    std::wstring command=L"\""+exe+L"\" \""+dllPath()+L"\",WaterWorker "+std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION p{};
    if(CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root().c_str(),&startup,&p)){CloseHandle(p.hThread);CloseHandle(p.hProcess);}else log("Could not start native water worker");return 0;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){ownModule=module;wchar_t path[32768];GetModuleFileNameW(nullptr,path,32768);const wchar_t* name=wcsrchr(path,L'\\');name=name?name+1:path;
        if(!_wcsicmp(name,L"Cube.exe")||!_wcsicmp(name,L"Cube-SteamWater-Preview.exe")){HANDLE thread=CreateThread(nullptr,0,startWorker,nullptr,0,nullptr);if(thread)CloseHandle(thread);}
    }return TRUE;
}





