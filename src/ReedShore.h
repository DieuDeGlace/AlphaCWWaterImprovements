#pragma once
#include <cmath>
#include <cstdint>
struct ShoreCell {bool valid=false;int ground=-10000,groundType=0,water=-10000,depth=0;bool rock=false;};
static bool dryBank(const ShoreCell& cell){return cell.valid&&!cell.rock&&cell.depth==0&&cell.groundType==4;}
static bool reedPatchCell(int x,int y){
 int tx=x/10,ty=y/10;uint32_t hash=uint32_t(tx)*73856093u^uint32_t(ty)*19349663u;
 int ax=tx*10+3+int(hash%3),ay=ty*10+3+int((hash>>8)%3);
 int dx=x-ax,dy=y-ay;return dx*dx+dy*dy<=9;
}
template<class Cell,class Occupied>static bool chooseReedBank(int x,int y,int chunkX,int chunkY,Cell cell,Occupied occupied,int& outX,int& outY,int& outZ){
 const auto source=cell(x,y);if(!source.valid||source.depth<1)return false;
 int best=0x7fffffff;bool found=false;
 for(int ny=y-48;ny<=y+48;ny++)for(int nx=x-48;nx<=x+48;nx++){
  if(!reedPatchCell(nx,ny)||occupied(nx,ny))continue;auto c=cell(nx,ny);
  if(!dryBank(c)||c.ground<source.water-8||c.ground>source.water+8)continue;
  bool shore=false;for(int dy=-4;dy<=4&&!shore;dy++)for(int dx=-4;dx<=4;dx++){auto q=cell(nx+dx,ny+dy);if(q.valid&&q.depth>0&&std::abs(q.water-c.ground)<=8){shore=true;break;}}if(!shore)continue;
  int dx=nx-x,dy=ny-y;int score=(dx*dx+dy*dy)*4+std::abs(c.ground-source.water)*2;
  if(score<best){best=score;outX=nx;outY=ny;outZ=c.ground+1;found=true;}
 }
 return found;
}


