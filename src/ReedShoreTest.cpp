#include "ReedShore.h"
#include <cassert>
#include <set>
#include <cstdio>
int main(){auto cell=[](int x,int y){if(y<=8)return ShoreCell{true,2,6,9,7};return ShoreCell{true,10+(y/4)%2,4,-10000,0,x>=10&&x<=18&&y>=9&&y<=17};};std::set<std::pair<int,int>> used;auto occupied=[&](int x,int y){for(auto p:used){int dx=x-p.first,dy=y-p.second;if(dx*dx+dy*dy<1.1f*1.1f)return true;}return false;};for(int i=0;i<8;i++){int x,y,z;assert(chooseReedBank(12,8,0,0,cell,occupied,x,y,z));assert(reedPatchCell(x,y));assert(dryBank(cell(x,y)));assert(y<=12);assert(z==cell(x,y).ground+1);used.insert({x,y});}assert(!dryBank(ShoreCell{true,10,6,-10000,0}));assert(!dryBank(ShoreCell{true,10,7,-10000,0}));auto raised=[&](int x,int y){auto c=cell(x,y);if(x==12&&y==8)c.water=17;return c;};int ax,ay,az;assert(chooseReedBank(12,8,0,0,raised,[](int,int){return false;},ax,ay,az));int x,y,z;assert(!chooseReedBank(12,20,0,0,cell,occupied,x,y,z));auto rocks=[](int,int){return ShoreCell{true,10,4,-10000,0,true};};assert(!chooseReedBank(12,8,0,0,rocks,occupied,x,y,z));puts("PASS: 8 reeds in shoreline patches, rock exclusion, sloping soil, spacing, dry-source rejection");}



