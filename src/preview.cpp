#include "preview.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>

namespace {
using Color = std::array<uint8_t,3>;
const std::array<Color,8> colors{{{0,255,255},{255,153,51},{102,255,102},{255,102,255},
                                 {255,255,51},{102,153,255},{255,102,102},{255,255,255}}};
// A small fixed font is sufficient for diagnostic titles, indices and ticks.
const char* glyph(char ch) {
    static const char* letters[] = {
        "01110100011000111111100011000110001", "11110100011000111110100011000111110",
        "01111100001000010000100001000001111", "11110100011000110001100011000111110",
        "11111100001000011110100001000011111", "11111100001000011110100001000010000",
        "01111100001000010111100011000101111", "10001100011000111111100011000110001",
        "11111001000010000100001000010011111", "00111000100001000010100101001001100",
        "10001100101010011000101001001010001", "10000100001000010000100001000011111",
        "10001110111010110101100011000110001", "10001110011010110011100011000110001",
        "01110100011000110001100011000101110", "11110100011000111110100001000010000",
        "01110100011000110001101011001001101", "11110100011000111110101001001010001",
        "01111100001000001110000010000111110", "11111001000010000100001000010000100",
        "10001100011000110001100011000101110", "10001100011000110001100010101000100",
        "10001100011000110101101011010101010", "10001100010101000100010101000110001",
        "10001100010101000100001000010000100", "11111000010001000100010001000011111"};
    static const char* digits[] = {
        "01110100011001110101110011000101110", "00100011000010000100001000010001110",
        "01110100010000100010001000100011111", "11110000010000101110000010000111110",
        "00010001100101010010111110001000010", "11111100001000011110000010000111110",
        "01110100001000011110100011000101110", "11111000010001000100010000100001000",
        "01110100011000101110100011000101110", "01110100011000101111000010000101110"};
    if(ch>='a' && ch<='z') ch=static_cast<char>(ch-'a'+'A');
    if(ch>='A' && ch<='Z') return letters[ch-'A'];
    if(ch>='0' && ch<='9') return digits[ch-'0'];
    switch(ch) {
        case ' ': return "00000000000000000000000000000000000";
        case '=': return "00000000001111100000111110000000000";
        case '-': return "00000000000000011111000000000000000";
        case '_': return "00000000000000000000000000000011111";
        case '.': return "00000000000000000000000000110001100";
        case ':': return "00000011000110000000011000110000000";
        case '/': return "00001000100001000100010000100010000";
        default: return "01110100010000100010001000000000100";
    }
}
struct Image {
    int w,h; std::vector<uint8_t> rgb;
    Image(int width,int height):w(width),h(height),rgb(static_cast<size_t>(w)*h*3,20) {}
    void pixel(int x,int y,Color c) {
        if(x<0 || x>=w || y<0 || y>=h) return;
        size_t p=(static_cast<size_t>(y)*w+x)*3;
        std::copy(c.begin(),c.end(),rgb.begin()+p);
    }
    void fill(int x,int y,int width,int height,Color c) {
        for(int v=y;v<y+height;++v) for(int u=x;u<x+width;++u) pixel(u,v,c);
    }
    void line(int x0,int y0,int x1,int y1,Color c) {
        int dx=std::abs(x1-x0),dy=-std::abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
        for(;;) {
            pixel(x0,y0,c); if(x0==x1 && y0==y1) break;
            int e=2*err; if(e>=dy) {err+=dy;x0+=sx;} if(e<=dx) {err+=dx;y0+=sy;}
        }
    }
    void text(int x,int y,const std::string& s,int scale,Color c) {
        for(char ch:s) {
            const char* g=glyph(ch);
            for(int v=0;v<7;++v) for(int u=0;u<5;++u) if(g[v*5+u]=='1') fill(x+u*scale,y+v*scale,scale,scale,c);
            x+=6*scale;
        }
    }
    void badge(int x,int y,int index,Color c) {
        auto s=std::to_string(index);int width=static_cast<int>(s.size())*30+10;
        x=std::clamp(x,0,w-width);y=std::clamp(y,0,h-45);
        fill(x,y,width,45,{0,0,0});text(x+5,y+5,s,5,c);
    }
};
void u32(std::vector<uint8_t>& out,uint32_t v) {
    for(int shift=24;shift>=0;shift-=8) out.push_back(static_cast<uint8_t>(v>>shift));
}
uint32_t crc(const uint8_t* p,size_t n) {
    uint32_t v=0xffffffff;
    for(size_t i=0;i<n;++i) {
        v^=p[i];for(int k=0;k<8;++k) v=(v>>1)^(0xedb88320u & (0u-(v&1)));
    }
    return v^0xffffffff;
}
void chunk(std::ofstream& out,const char* type,const std::vector<uint8_t>& data) {
    std::vector<uint8_t> bytes;u32(bytes,static_cast<uint32_t>(data.size()));
    bytes.insert(bytes.end(),type,type+4);bytes.insert(bytes.end(),data.begin(),data.end());
    u32(bytes,crc(bytes.data()+4,bytes.size()-4));
    out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
}
// PNG with stored DEFLATE blocks: portable, fast, and no compression library.
void png(const std::filesystem::path& path,const Image& image) {
    std::vector<uint8_t> raw;raw.reserve(image.rgb.size()+image.h);
    for(int y=0;y<image.h;++y) {
        raw.push_back(0);auto row=image.rgb.begin()+static_cast<size_t>(y)*image.w*3;
        raw.insert(raw.end(),row,row+image.w*3);
    }
    std::vector<uint8_t> z{0x78,0x01};uint32_t a=1,b=0;
    for(uint8_t v:raw) {a=(a+v)%65521;b=(b+a)%65521;}
    for(size_t p=0;p<raw.size();) {
        auto n=static_cast<uint16_t>(std::min<size_t>(65535,raw.size()-p));
        z.push_back(p+n==raw.size()?1:0);
        z.push_back(static_cast<uint8_t>(n));z.push_back(static_cast<uint8_t>(n>>8));
        auto inv=static_cast<uint16_t>(~n);z.push_back(static_cast<uint8_t>(inv));z.push_back(static_cast<uint8_t>(inv>>8));
        z.insert(z.end(),raw.begin()+p,raw.begin()+p+n);p+=n;
    }
    u32(z,(b<<16)|a);
    std::ofstream out(path,std::ios::binary);if(!out) throw std::runtime_error("Cannot create box preview");
    const uint8_t signature[]={137,80,78,71,13,10,26,10};out.write(reinterpret_cast<const char*>(signature),8);
    std::vector<uint8_t> header;u32(header,image.w);u32(header,image.h);
    header.insert(header.end(),{8,2,0,0,0});chunk(out,"IHDR",header);chunk(out,"IDAT",z);chunk(out,"IEND",{});
    out.close();if(!out) throw std::runtime_error("Box preview write failed");
}
struct Panel {
    std::string title;int u,v,fixed_axis,fixed;double u0,u1,v0,v1;
    std::vector<size_t> indices;
    int width=414,height=286;
};
}
void write_box_preview(const std::filesystem::path& path,const std::array<int,3>& size,
                       const std::vector<PreviewBox>& boxes,const std::function<int(int,int,int)>& read,
                       const std::function<void()>& release,double foreground_threshold) {
    std::vector<size_t> all(boxes.size());for(size_t i=0;i<all.size();++i) all[i]=i;
    int x=size[0]/2,z=size[2]/2;
    if(!boxes.empty()) { x=(boxes[0].lo[0]+boxes[0].hi[0])/2;z=(boxes[0].lo[2]+boxes[0].hi[2])/2; }
    double start_window=std::max(1.0,size[2]*0.08),end_start=size[2]-start_window;
    for(const auto& b:boxes) {
        start_window=std::max(start_window,b.lo[2]+size[2]*0.02);
        end_start=std::min(end_start,b.hi[2]-size[2]*0.02);
    }
    start_window=std::min(start_window,double(size[2]));end_start=std::max(0.0,end_start);
    std::vector<Panel> panels{
        {"XY  Z="+std::to_string(z),0,1,2,z,0,double(size[0]),0,double(size[1]),all},
        {"YZ  X="+std::to_string(x),1,2,0,x,0,double(size[1]),0,double(size[2]),all},
        {"YZ  START ENDS",1,2,0,x,0,double(size[1]),0,start_window,all},
        {"YZ  FAR ENDS",1,2,0,x,0,double(size[1]),end_start,double(size[2]),all}};
    for(size_t i=0;i<boxes.size();++i) {
        int y=(boxes[i].lo[1]+boxes[i].hi[1])/2;
        panels.push_back({"XZ  BOX "+std::to_string(i+1)+"  Y="+std::to_string(y),0,2,1,y,0,double(size[0]),0,double(size[2]),{i}});
    }
    constexpr int pw=500,ph=410,top=90,left=62,plotw=414,ploth=286;
    panels[0].width=std::max(1,std::min(plotw,static_cast<int>(std::lround(ploth*double(size[0])/size[1]))));
    panels[0].height=std::max(1,std::min(ploth,static_cast<int>(std::lround(plotw*double(size[1])/size[0]))));
    int rows=static_cast<int>((panels.size()+2)/3);Image image(pw*3,top+ph*rows);
    image.text(24,18,"BOXES IN ORIGINAL VOXEL COORDINATES",2,{230,230,230});
    image.text(24,44,"1 = HIGHEST Y / NUMBERS MATCH BOXES.JSON",2,{230,230,230});
    std::vector<std::vector<int>> samples;std::vector<int> histogram;
    for(const auto& p:panels) {
        int plotw=p.width,ploth=p.height;
        samples.emplace_back(static_cast<size_t>(plotw)*ploth);
        auto& values=samples.back();
        for(int py=0;py<ploth;++py) {
            double v=p.v1-(py+0.5)*(p.v1-p.v0)/ploth;
            for(int px=0;px<plotw;++px) {
                double u=p.u0+(px+0.5)*(p.u1-p.u0)/plotw;
                std::array<int,3> at{};at[p.fixed_axis]=p.fixed;
                at[p.u]=std::clamp(static_cast<int>(u),0,size[p.u]-1);at[p.v]=std::clamp(static_cast<int>(v),0,size[p.v]-1);
                int value=std::max(0,read(at[0],at[1],at[2]));values[static_cast<size_t>(py)*plotw+px]=value;
                if(value>std::max(0.0,foreground_threshold)) histogram.push_back(value);
            }
            if(py%32==31) release();
        }
        release();
    }
    int high=1;
    if(!histogram.empty()) {
        size_t p=(histogram.size()-1)/2;
        std::nth_element(histogram.begin(),histogram.begin()+p,histogram.end());high=std::max(1,2*histogram[p]);
    }
    image.text(24,67,"GRAY RANGE: 0 TO "+std::to_string(high)+" / BOXES ARE FINAL CROP BOUNDS",1,{180,180,180});
    for(size_t i=0;i<panels.size();++i) {
        const auto& p=panels[i];int ox=static_cast<int>(i%3)*pw,oy=top+static_cast<int>(i/3)*ph;
        int plotw=p.width,ploth=p.height;
        int bx=ox+left+(414-plotw)/2,by=oy+38+(286-ploth)/2;
        image.text(ox+14,oy+6,p.title,2,{230,230,230});
        for(int y=0;y<ploth;++y) for(int x=0;x<plotw;++x) {
            int value=samples[i][static_cast<size_t>(y)*plotw+x];
            auto gray=static_cast<uint8_t>(std::min(255,static_cast<int>(255.0*value/high)));
            image.pixel(bx+x,by+y,{gray,gray,gray});
        }
        auto ux=[&](double u){return bx+static_cast<int>(std::lround((u-p.u0)/(p.u1-p.u0)*plotw));};
        auto vy=[&](double v){return by+static_cast<int>(std::lround((p.v1-v)/(p.v1-p.v0)*ploth));};
        for(int tick=0;tick<=4;++tick) {
            double u=p.u0+(p.u1-p.u0)*tick/4,v=p.v0+(p.v1-p.v0)*tick/4;
            int tx=ux(u),ty=vy(v);
            image.line(tx,by+ploth,tx,by+ploth+5,{180,180,180});
            image.text(tx-12,by+ploth+10,std::to_string(static_cast<int>(u)),1,{180,180,180});
            image.line(bx-5,ty,bx,ty,{180,180,180});
            image.text(ox+5,ty-4,std::to_string(static_cast<int>(v)),1,{180,180,180});
        }
        const char axes[]="XYZ";
        image.text(bx+plotw/2-6,by+ploth+26,std::string(1,axes[p.u]),2,{230,230,230});
        image.text(ox+8,oy+20,std::string(1,axes[p.v]),2,{230,230,230});
        for(size_t index:p.indices) {
            const auto& b=boxes[index];Color color=colors[index%colors.size()];
            double u0=std::max(p.u0,double(b.lo[p.u])),u1=std::min(p.u1,double(b.hi[p.u]));
            double v0=std::max(p.v0,double(b.lo[p.v])),v1=std::min(p.v1,double(b.hi[p.v]));
            if(u1<=u0 || v1<=v0) continue;
            int x0=ux(u0),x1=ux(u1),y0=vy(v1),y1=vy(v0);
            for(int t=0;t<3;++t) {
                if(b.hi[p.v]<=p.v1) image.line(x0,y0+t,x1,y0+t,color);
                if(b.lo[p.v]>=p.v0) image.line(x0,y1-t,x1,y1-t,color);
                if(b.lo[p.u]>=p.u0) image.line(x0+t,y0,x0+t,y1,color);
                if(b.hi[p.u]<=p.u1) image.line(x1-t,y0,x1-t,y1,color);
            }
            int badge_width=static_cast<int>(std::to_string(index+1).size())*30+10;
            int labelx=std::clamp((x0+x1)/2-badge_width/2,bx,bx+std::max(0,plotw-badge_width));
            int labely=std::clamp((y0+y1)/2-22,by,by+std::max(0,ploth-45));
            image.badge(labelx,labely,static_cast<int>(index+1),color);
        }
        if(p.indices.size()==1) {
            std::string label=boxes[p.indices.front()].name;if(label.size()>58) label=label.substr(0,55)+"...";
            image.text(ox+14,oy+ph-25,label,1,colors[p.indices.front()%colors.size()]);
        }
    }
    png(path,image);
}
