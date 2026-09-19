#include "preview.h"
#include "json.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using I3 = std::array<int, 3>;
using D3 = std::array<double, 3>;
using Run = std::pair<int, int>; // half open
struct Box { I3 lo{}, hi{}; std::string name; };
struct Config {
    fs::path input, output, labels_file;
    int count = 0, samples = 3, cross_sections = 9, step = 2, z_step = 4;
    double threshold = 200, metal_threshold = 0, erosion = 1.5, margin = 5;
    double z_margin = 3, end_trim = 0, gap = 20, plate_thickness = 20, min_width = 10, min_length = 100;
    bool preview = false, end_guard = false, box_preview = true;
    std::vector<std::string> labels;
    std::vector<Box> boxes;
};
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b-a+1);
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out; std::istringstream in(s); std::string item;
    while (std::getline(in, item, sep)) out.push_back(trim(item));
    return out;
}
double number(const std::string& s) {
    size_t used = 0; double v = std::stod(s, &used);
    if (used != s.size() || !std::isfinite(v)) throw std::runtime_error("Invalid number: " + s);
    return v;
}
int integer(const std::string& s) {
    double v = number(s);
    if (v != std::floor(v) || v < 0 || v > std::numeric_limits<int>::max())
        throw std::runtime_error("Expected nonnegative integer: " + s);
    return static_cast<int>(v);
}
std::string safe_name(const std::string& s) {
    if (s.empty() || s == "." || s == "..") throw std::runtime_error("Empty or invalid output label");
    for (unsigned char c : s)
        if (c < 32 || std::string("<>:\"/\\|?*").find(c) != std::string::npos)
            throw std::runtime_error("Label contains an unsafe filename character: " + s);
    return s;
}
const char* help = R"(Usage: scan_separator INPUT.nrrd [options]
Separates long beams arranged along Y. Output order: decreasing voxel Y.
  -o, --output DIR          Output directory (default: INPUT_stem_crops)
  -n, --count N             Expected count; mismatch is an error
  --labels A,B,C            Names in negative Y order; implies count
  --labels-file FILE        Names separated by commas or newlines
  --preview                Write JSON and box image, without the large crops
  --no-box-preview         Skip the default boxes_preview.png diagnostic
  --threshold VALUE        Foreground lower threshold (default 200)
  --metal-threshold VALUE  Reject values >= VALUE (default disabled)
  --erosion-mm RADIUS      2D rectangular erosion radius (default 1.5 mm)
  --margin-mm VALUE        X/Y crop margin (default 5 mm)
  --z-margin-mm VALUE      End margin (default 3 mm)
  --end-trim-mm VALUE      Additional inward trim at both ends (default 0)
  --trim-end-clutter       Opt in to trimming for detached end components
  --keep-end-clutter       Keep full ends (default; accepts holder overlap)
  --gap-mm VALUE           Bridge internal length-profile gaps (default 20)
  --plate-thickness-mm V   Ignore detached fragments <= V long (default 20)
  --min-width-mm VALUE     Minimum beam width in Y (default 10)
  --min-length-mm VALUE    Minimum beam length (default 100)
  --samples N              YZ/XZ planes per beam (default 3)
  --cross-sections N       XY seed planes over full Z (default 9)
  --step N                 X/Y sampling stride (default 2)
  --z-step N               Z sampling stride (default 4)
  --box x0,y0,z0,x1,y1,z1  Use an explicit half-open box; repeat to bypass detection
  -h, --help               Show help
Input: raw 3D uint16 or int16 NRRD, attached or a single detached data file.
Crops retain the source voxel type and byte order. Existing outputs are refused.
Physical parameters use header spacing, assumed to be in millimetres.
If OUTPUT contains boxes.json but no NRRD crops, its edited boxes are replayed.
)";
Config parse(int argc, char** argv) {
    Config c;
    for (int i=1; i<argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() { if (++i >= argc) throw std::runtime_error("Missing value for " + a); return std::string(argv[i]); };
        if (a == "--help" || a == "-h") { std::cout << help; std::exit(0); }
        else if (a == "--output" || a == "-o") c.output = value();
        else if (a == "--count" || a == "-n") { c.count = integer(value()); if (!c.count) throw std::runtime_error("Count must be positive"); }
        else if (a == "--labels") c.labels = split(value(), ',');
        else if (a == "--labels-file") c.labels_file = value();
        else if (a == "--preview") c.preview = true;
        else if (a == "--no-box-preview") c.box_preview = false;
        else if (a == "--trim-end-clutter") c.end_guard = true;
        else if (a == "--keep-end-clutter") c.end_guard = false;
        else if (a == "--threshold") c.threshold = number(value());
        else if (a == "--metal-threshold") c.metal_threshold = number(value());
        else if (a == "--erosion-mm") c.erosion = number(value());
        else if (a == "--margin-mm") c.margin = number(value());
        else if (a == "--z-margin-mm") c.z_margin = number(value());
        else if (a == "--end-trim-mm") c.end_trim = number(value());
        else if (a == "--gap-mm") c.gap = number(value());
        else if (a == "--plate-thickness-mm") c.plate_thickness = number(value());
        else if (a == "--min-width-mm") c.min_width = number(value());
        else if (a == "--min-length-mm") c.min_length = number(value());
        else if (a == "--samples") c.samples = integer(value());
        else if (a == "--cross-sections") c.cross_sections = integer(value());
        else if (a == "--step") c.step = integer(value());
        else if (a == "--z-step") c.z_step = integer(value());
        else if (a == "--box") {
            auto p = split(value(), ','); if (p.size()!=6) throw std::runtime_error("--box needs six integers");
            Box b; for (int d=0; d<3; ++d) { b.lo[d]=integer(p[d]); b.hi[d]=integer(p[d+3]); } c.boxes.push_back(b);
        } else if (!a.empty() && a[0]=='-') throw std::runtime_error("Unknown option: " + a);
        else if (c.input.empty()) c.input = a;
        else throw std::runtime_error("Only one input scan is accepted per command");
    }
    if (c.input.empty()) throw std::runtime_error(help);
    if (!c.labels_file.empty()) {
        if (!c.labels.empty()) throw std::runtime_error("Use either --labels or --labels-file");
        std::ifstream in(c.labels_file); if (!in) throw std::runtime_error("Cannot read labels file");
        std::string line; while (std::getline(in, line)) if (!trim(line).empty()) {
            auto names = split(line, ','); c.labels.insert(c.labels.end(), names.begin(), names.end());
        }
    }
    if (!c.labels.empty()) {
        if (c.count && c.count != static_cast<int>(c.labels.size())) throw std::runtime_error("Count and number of labels disagree");
        c.count = static_cast<int>(c.labels.size());
    }
    if (!c.samples || c.samples>31 || !c.cross_sections || c.cross_sections>1001 || !c.step || !c.z_step)
        throw std::runtime_error("Samples must be 1..31, cross-sections 1..1001, and strides positive");
    for (double v : {c.erosion,c.margin,c.z_margin,c.end_trim,c.gap,c.plate_thickness,c.min_width,c.min_length,c.metal_threshold})
        if (v < 0) throw std::runtime_error("Physical parameters must be nonnegative");
    if (c.metal_threshold && c.metal_threshold <= c.threshold) throw std::runtime_error("Metal threshold must exceed foreground threshold");
    if (c.output.empty()) c.output = c.input.parent_path() / (c.input.stem().string() + "_crops");
    return c;
}
D3 vector3(std::string text) {
    for (char& ch : text) if (ch=='(' || ch==')' || ch==',') ch=' ';
    std::istringstream in(text); D3 v; std::string extra;
    if (!(in>>v[0]>>v[1]>>v[2]) || (in>>extra)) throw std::runtime_error("Expected three coordinates: " + text);
    for (double n : v) if (!std::isfinite(n)) throw std::runtime_error("Non-finite coordinate");
    return v;
}
std::string vec_text(D3 v) {
    std::ostringstream out; out << std::setprecision(17) << '(' << v[0] << ',' << v[1] << ',' << v[2] << ')'; return out.str();
}
struct Volume {
    I3 size{}; D3 spacing{1,1,1}; std::array<D3,3> directions{};
    bool has_directions=false, signed_type=false, big_endian=false;
    std::map<std::string,std::string> fields;
    std::vector<std::string> extras;
    std::string original_header;
    fs::path data_path;
    size_t offset=0, bytes=0, mapped_size=0;
    int fd=-1; void* mapping=MAP_FAILED; const uint8_t* data=nullptr;
    explicit Volume(const fs::path& path) {
        std::ifstream in(path,std::ios::binary); if (!in) throw std::runtime_error("Cannot open " + path.string());
        std::string line; if (!std::getline(in,line) || trim(line).rfind("NRRD000",0)!=0) throw std::runtime_error("Not a NRRD file");
        original_header = trim(line) + "\n";
        bool ended=false;
        while (std::getline(in,line)) {
            if (!line.empty() && line.back()=='\r') line.pop_back();
            original_header += line + "\n";
            if (line.empty()) { ended=true; break; }
            if (original_header.size()>1024*1024) throw std::runtime_error("NRRD header exceeds 1 MiB");
            if (line[0]=='#' || line.find(":=")!=std::string::npos) { extras.push_back(line); continue; }
            auto colon=line.find(':'); if (colon==std::string::npos) throw std::runtime_error("Invalid NRRD header line");
            std::string key=lower(trim(line.substr(0,colon)));
            if (!fields.emplace(key,trim(line.substr(colon+1))).second) throw std::runtime_error("Duplicate NRRD field: " + key);
        }
        if (!ended) throw std::runtime_error("NRRD header needs a blank line terminator");
        offset=static_cast<size_t>(in.tellg()); data_path=path;
        if (fields["dimension"]!="3") throw std::runtime_error("Only 3D NRRD images are supported");
        if (lower(fields["encoding"])!="raw") throw std::runtime_error("Input encoding must be raw; decompress the NRRD first");
        std::string type=lower(fields["type"]);
        if (type=="uint16" || type=="uint16_t" || type=="ushort" || type=="unsigned short" || type=="unsigned short int") signed_type=false;
        else if (type=="int16" || type=="int16_t" || type=="short" || type=="short int" || type=="signed short" || type=="signed short int") signed_type=true;
        else throw std::runtime_error("Only uint16 and int16 are supported");
        if (lower(fields["endian"])!="little" && lower(fields["endian"])!="big") throw std::runtime_error("NRRD requires endian: little or big");
        big_endian=lower(fields["endian"])=="big";
        std::istringstream dims(fields["sizes"]); std::string token;
        for (int& n:size) { if (!(dims>>token)) throw std::runtime_error("Invalid sizes"); n=integer(token); if (!n) throw std::runtime_error("Zero dimension"); }
        if (dims>>token) throw std::runtime_error("Expected exactly three sizes");
        bytes=2;
        for (int n:size) { if (bytes>std::numeric_limits<size_t>::max()/static_cast<size_t>(n)) throw std::runtime_error("Volume size overflow"); bytes*=n; }
        if (fields.count("spacings")) spacing=vector3(fields.at("spacings"));
        if (fields.count("space directions")) {
            std::string s=fields.at("space directions"); size_t pos=0;
            for (int d=0; d<3; ++d) {
                auto a=s.find('(',pos), b=s.find(')',pos);
                if (a==std::string::npos || b==std::string::npos || b<a || !trim(s.substr(pos,a-pos)).empty())
                    throw std::runtime_error("Three spatial direction vectors are required");
                directions[d]=vector3(s.substr(a,b-a+1)); pos=b+1;
                spacing[d]=std::sqrt(std::inner_product(directions[d].begin(),directions[d].end(),directions[d].begin(),0.0));
            }
            if (!trim(s.substr(pos)).empty()) throw std::runtime_error("Invalid space directions");
            has_directions=true;
        }
        for (double& v:spacing) { v=std::abs(v); if (!(v>0)) throw std::runtime_error("Voxel spacing must be finite and nonzero"); }
        if (!fields.count("spacings") && !has_directions) std::cerr << "Warning: no spacing; assuming 1 mm voxels\n";
        if (fields.count("space origin")) {
            vector3(fields.at("space origin"));
            if (!has_directions) throw std::runtime_error("space origin requires space directions for correct cropping");
        }
        for (const char* key : {"units","space units"}) if (fields.count(key)) {
            auto units=fields.at(key);
            for (const auto& u:split(units,' ')) if (!u.empty() && u!="\"mm\"")
                throw std::runtime_error("Physical options require millimetre units; unsupported " + std::string(key));
        }
        if (fields.count("data file") || fields.count("datafile")) {
            std::string name=fields.count("data file")?fields.at("data file"):fields.at("datafile");
            if (name.rfind("LIST",0)==0 || name.find('%')!=std::string::npos) throw std::runtime_error("Only a single detached raw file is supported");
            if (name.size()>1 && name.front()=='\"' && name.back()=='\"') name=name.substr(1,name.size()-2);
            data_path=path.parent_path()/name; offset=0;
        }
        if (fields.count("line skip") && fields.at("line skip")!="0") throw std::runtime_error("line skip is unsupported");
        long long skip=0;
        if (fields.count("byte skip")) { size_t used; skip=std::stoll(fields.at("byte skip"),&used); if (used!=fields.at("byte skip").size() || skip < -1) throw std::runtime_error("Invalid byte skip"); }
        auto file_size=fs::file_size(data_path);
        if (file_size>std::numeric_limits<size_t>::max()) throw std::runtime_error("File too large to map");
        mapped_size=static_cast<size_t>(file_size);
        if (skip==-1) { if (mapped_size<bytes) throw std::runtime_error("Truncated raw data"); offset=mapped_size-bytes; }
        else { if (static_cast<size_t>(skip)>mapped_size || offset>mapped_size-static_cast<size_t>(skip)) throw std::runtime_error("Invalid data offset"); offset+=static_cast<size_t>(skip); }
        if (offset>mapped_size || bytes>mapped_size-offset) throw std::runtime_error("Truncated NRRD voxel data");
        fd=::open(data_path.c_str(),O_RDONLY); if (fd<0) throw std::runtime_error("Cannot open voxel data");
        mapping=mmap(nullptr,mapped_size,PROT_READ,MAP_PRIVATE,fd,0);
        if (mapping==MAP_FAILED) { ::close(fd); fd=-1; throw std::runtime_error("Cannot memory-map voxel data"); }
        data=static_cast<const uint8_t*>(mapping)+offset;
    }
    ~Volume() { if(mapping!=MAP_FAILED) munmap(mapping,mapped_size); if(fd>=0) ::close(fd); }
    Volume(const Volume&)=delete; Volume& operator=(const Volume&)=delete;
    size_t index(int x,int y,int z) const { return (static_cast<size_t>(z)*size[1]+y)*size[0]+x; }
    // Drop mapped pages already consumed, without evicting the shared OS file
    // cache. Keep source RSS bounded even when the scan is larger than RAM.
    void release_z(int lo,int hi) const {
        size_t begin=offset+2*index(0,0,lo),end=offset+2*index(0,0,hi);
        size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
        begin-=begin%page;
        if(end>begin) madvise(static_cast<uint8_t*>(mapping)+begin,end-begin,MADV_DONTNEED);
    }
    int value(int x,int y,int z) const {
        const uint8_t* p=data+2*index(x,y,z);
        unsigned v=big_endian?(unsigned(p[0])*256+p[1]):(unsigned(p[1])*256+p[0]);
        return signed_type && v>=32768 ? static_cast<int>(v)-65536 : static_cast<int>(v);
    }
    std::string header(const Box& box) const {
        auto f=fields; f["type"]=signed_type?"int16":"uint16"; f["encoding"]="raw";
        for (auto key:{"data file","datafile","byte skip","line skip","min","max","old min","old max","content","number"}) f.erase(key);
        std::ostringstream sizes; sizes << box.hi[0]-box.lo[0] << ' ' << box.hi[1]-box.lo[1] << ' ' << box.hi[2]-box.lo[2]; f["sizes"]=sizes.str();
        if (has_directions && f.count("space origin")) {
            D3 origin=vector3(f.at("space origin"));
            for(int d=0;d<3;++d) for(int j=0;j<3;++j) origin[j]+=box.lo[d]*directions[d][j];
            f["space origin"]=vec_text(origin);
        }
        // Axis-coordinate endpoints shift by their own coordinate increment.
        if (f.count("axis mins") || f.count("axis maxs")) {
            D3 increment=fields.count("spacings")?vector3(fields.at("spacings")):spacing;
            for (auto key:{"axis mins","axis maxs"}) if(f.count(key)) {
                D3 v=vector3(f.at(key)); std::ostringstream out; out<<std::setprecision(17);
                for(int d=0;d<3;++d) { v[d]+=(std::string(key)=="axis mins"?box.lo[d]:box.hi[d]-size[d])*increment[d]; if(d) out<<' '; out<<v[d]; }
                f[key]=out.str();
            }
        }
        std::ostringstream out; out << "NRRD0005\n";
        // Required ordering: dimension precedes per-axis fields; space precedes spatial fields.
        for(auto key:{"type","dimension","sizes","encoding","endian","space","space dimension"})
            if(f.count(key)) { out<<key<<": "<<f.at(key)<<'\n'; f.erase(key); }
        for(auto& field:f) out<<field.first<<": "<<field.second<<'\n';
        for(auto& line:extras) out<<line<<'\n';
        out << "scan_separator_original_min:=" << box.lo[0]<<','<<box.lo[1]<<','<<box.lo[2]<<"\n\n";
        return out.str();
    }
};
struct Mask {
    int w,h; std::vector<uint8_t> pixels;
    Mask(int width,int height):w(width),h(height),pixels(static_cast<size_t>(w)*h) {}
    uint8_t& at(int x,int y) { return pixels[static_cast<size_t>(y)*w+x]; }
};
int cells(int n,int stride) { return 1+(n-1)/stride; }
int px(double mm,double spacing) {
    double n=std::ceil(mm/spacing);
    if(n>std::numeric_limits<int>::max()/4) throw std::runtime_error("Physical parameter too large");
    return static_cast<int>(n);
}
// Integral-image erosion: constant work per pixel regardless of kernel size.
Mask erode(const Mask& m,int rx,int ry) {
    if(!rx && !ry) return m;
    Mask out(m.w,m.h);
    if(rx>=m.w || ry>=m.h) return out;
    size_t pitch=static_cast<size_t>(m.w)+1;
    std::vector<uint64_t> sum(pitch*(static_cast<size_t>(m.h)+1));
    for(int y=0;y<m.h;++y) {
        uint64_t row=0;
        for(int x=0;x<m.w;++x) { row+=m.pixels[static_cast<size_t>(y)*m.w+x]; sum[(y+1)*pitch+x+1]=sum[y*pitch+x+1]+row; }
    }
    uint64_t area=static_cast<uint64_t>(2*rx+1)*(2*ry+1);
    for(int y=ry;y<m.h-ry;++y) for(int x=rx;x<m.w-rx;++x) {
        size_t x0=x-rx,x1=x+rx+1,y0=y-ry,y1=y+ry+1;
        auto n=sum[y1*pitch+x1]+sum[y0*pitch+x0]-sum[y0*pitch+x1]-sum[y1*pitch+x0];
        out.at(x,y)=n==area;
    }
    return out;
}
bool foreground(int value,const Config& c) { return value>c.threshold && (!c.metal_threshold || value<c.metal_threshold); }
std::vector<Run> runs(const std::vector<uint8_t>& m) {
    std::vector<Run> out; int start=-1;
    for(int i=0;i<=static_cast<int>(m.size());++i) {
        bool on=i<static_cast<int>(m.size()) && m[i];
        if(on && start<0) start=i;
        if(!on && start>=0) { out.emplace_back(start,i); start=-1; }
    }
    return out;
}
void bridge(std::vector<uint8_t>& m,int max_gap) {
    auto r=runs(m);
    for(size_t i=1;i<r.size();++i) if(r[i].first-r[i-1].second<=max_gap)
        std::fill(m.begin()+r[i-1].second,m.begin()+r[i].first,1);
}
std::vector<int> positions(int lo,int hi,int n,double inset) {
    std::vector<int> out;
    for(int i=0;i<n;++i) {
        double f=n==1?0.5:inset+(1-2*inset)*i/(n-1);
        int p=lo+static_cast<int>(std::lround((hi-lo-1)*f));
        if(out.empty() || out.back()!=p) out.push_back(p);
    }
    return out;
}
struct Seed { int y0,y1,x0,x1; };
std::vector<Seed> seeds(const Volume& v,const Config& c) {
    int nx=cells(v.size[0],c.step), ny=cells(v.size[1],c.step);
    std::vector<int> votes(ny);
    std::vector<std::vector<int>> xlo(ny), xhi(ny);
    int rx=px(c.erosion,v.spacing[0]*c.step),ry=px(c.erosion,v.spacing[1]*c.step);
    auto zs=positions(0,v.size[2],c.cross_sections,0.08);
    for(int z:zs) {
        Mask mask(nx,ny);
        for(int y=0;y<ny;++y) for(int x=0;x<nx;++x) mask.at(x,y)=foreground(v.value(x*c.step,y*c.step,z),c);
        mask=erode(mask,rx,ry);
        std::vector<Run> row_runs(ny,{0,0});
        int peak=0;
        for(int y=0;y<ny;++y) {
            std::vector<uint8_t> row(mask.pixels.begin()+static_cast<size_t>(y)*nx,mask.pixels.begin()+static_cast<size_t>(y+1)*nx);
            for(auto r:runs(row)) if(r.second-r.first>row_runs[y].second-row_runs[y].first) row_runs[y]=r;
            peak=std::max(peak,row_runs[y].second-row_runs[y].first);
        }
        int cutoff=std::max(px(c.min_width,v.spacing[0]*c.step),static_cast<int>(std::ceil(peak*0.15)));
        for(int y=0;y<ny;++y) if(row_runs[y].second-row_runs[y].first>=cutoff && row_runs[y].second>row_runs[y].first) {
            ++votes[y]; xlo[y].push_back(row_runs[y].first); xhi[y].push_back(row_runs[y].second);
        }
    }
    std::vector<uint8_t> occupied(ny);
    int required=std::max(1,static_cast<int>((zs.size()+2)/3));
    for(int y=0;y<ny;++y) occupied[y]=votes[y]>=required;
    // Tiny holes in the transverse projection are not separate objects.
    bridge(occupied,px(c.erosion,v.spacing[1]*c.step));
    std::vector<Seed> out;
    for(auto [a,b]:runs(occupied)) if((b-a)*c.step*v.spacing[1]>=c.min_width) {
        std::vector<int> lows,highs;
        for(int y=a;y<b;++y) { lows.insert(lows.end(),xlo[y].begin(),xlo[y].end()); highs.insert(highs.end(),xhi[y].begin(),xhi[y].end()); }
        if(lows.empty()) continue;
        std::sort(lows.begin(),lows.end()); std::sort(highs.begin(),highs.end());
        size_t tail=lows.size()/50;
        int xmin=lows[tail],xmax=highs[highs.size()-1-tail];
        if(xmax>xmin) out.push_back({a*c.step,std::min(v.size[1],b*c.step),xmin*c.step,std::min(v.size[0],xmax*c.step)});
    }
    return out;
}
// Thin disconnected plates must be removed BEFORE gap closing; otherwise
// a short empty gap at the end could join a plate to the beam.
void components(Mask& m,int min_height,bool largest) {
    std::vector<uint8_t> visited(m.pixels.size());
    std::vector<size_t> queue,best;
    for(size_t start=0;start<m.pixels.size();++start) if(m.pixels[start] && !visited[start]) {
        queue.clear(); queue.push_back(start); visited[start]=1;
        int zlo=m.h,zhi=0;
        for(size_t i=0;i<queue.size();++i) {
            size_t p=queue[i]; int x=static_cast<int>(p%m.w),y=static_cast<int>(p/m.w);
            zlo=std::min(zlo,y); zhi=std::max(zhi,y+1);
            auto visit=[&](size_t q) { if(m.pixels[q] && !visited[q]) { visited[q]=1; queue.push_back(q); } };
            if(x) visit(p-1);
            if(x+1<m.w) visit(p+1);
            if(y) visit(p-m.w);
            if(y+1<m.h) visit(p+m.w);
        }
        if(zhi-zlo<=min_height) for(size_t p:queue) m.pixels[p]=0;
        else if(largest && queue.size()>best.size()) best=queue;
    }
    if(largest) {
        std::fill(m.pixels.begin(),m.pixels.end(),0);
        for(size_t p:best) m.pixels[p]=1;
    }
}
// Close short gaps at the same lateral position, then retain the beam.
void retain_beam(Mask& m,int gap,int plate_height) {
    components(m,plate_height,false);
    for(int u=0;u<m.w;++u) {
        int last=-1;
        for(int z=0;z<m.h;++z) if(m.at(u,z)) {
            if(last>=0 && z-last-1<=gap) for(int k=last+1;k<z;++k) m.at(u,k)=1;
            last=z;
        }
    }
    components(m,0,true);
}
struct Obstacle { int lo,hi,zlo,zhi; double fill; };
struct Measurement { int lateral_lo,lateral_hi,zlo,zhi; std::vector<Obstacle> obstacles; };
std::vector<Obstacle> detached(Mask raw,const Mask& beam,int band_lo,int step,int zstep,int radius,int rz,const Volume& v,int axis,const Config& c) {
    for(size_t p=0;p<raw.pixels.size();++p) raw.pixels[p]=raw.pixels[p] && !beam.pixels[p];
    std::vector<size_t> queue; std::vector<Obstacle> out;
    for(size_t start=0;start<raw.pixels.size();++start) if(raw.pixels[start]) {
        queue.clear(); queue.push_back(start); raw.pixels[start]=0;
        int lo=raw.w,hi=0,zlo=raw.h,zhi=0;
        for(size_t i=0;i<queue.size();++i) {
            size_t p=queue[i]; int x=static_cast<int>(p%raw.w),z=static_cast<int>(p/raw.w);
            lo=std::min(lo,x); hi=std::max(hi,x+1); zlo=std::min(zlo,z); zhi=std::max(zhi,z+1);
            auto visit=[&](size_t q) { if(raw.pixels[q]) { raw.pixels[q]=0; queue.push_back(q); } };
            if(x) visit(p-1);
            if(x+1<raw.w) visit(p+1);
            if(z) visit(p-raw.w);
            if(z+1<raw.h) visit(p+raw.w);
        }
        double area=queue.size()*v.spacing[axis]*step*v.spacing[2]*zstep;
        if((hi-lo)*step*v.spacing[axis]>=c.min_width && area>=c.min_width*c.min_width)
            out.push_back({std::max(0,band_lo+(lo-radius-1)*step),std::min(v.size[axis],band_lo+(hi+radius+1)*step),
                           std::max(0,(zlo-rz-1)*zstep),std::min(v.size[2],(zhi+rz+1)*zstep),static_cast<double>(queue.size())/((hi-lo)*(zhi-zlo))});
    }
    return out;
}
Measurement measure(const Volume& v,const Config& c,int lateral_axis,int band_lo,int band_hi,int sample_lo,int sample_hi) {
    int sample_axis=1-lateral_axis;
    int width=cells(band_hi-band_lo,c.step), height=cells(v.size[2],c.z_step);
    Mask fused(width,height);
    std::vector<uint8_t> votes(fused.pixels.size());
    auto ps=positions(sample_lo,sample_hi,c.samples,0.30);
    int radius=px(c.erosion,v.spacing[lateral_axis]*c.step),rz=px(c.erosion,v.spacing[2]*c.z_step);
    std::vector<Mask> planes;
    for(size_t i=0;i<ps.size();++i) planes.emplace_back(width,height);
    int released=0;
    for(int z=0;z<height;++z) {
        for(int u=0;u<width;++u) for(size_t i=0;i<ps.size();++i) {
            I3 at{}; at[lateral_axis]=band_lo+u*c.step; at[sample_axis]=ps[i]; at[2]=z*c.z_step;
            planes[i].at(u,z)=foreground(v.value(at[0],at[1],at[2]),c);
        }
        int consumed=std::min(v.size[2],z*c.z_step+1);
        if(consumed-released>=64) { v.release_z(released,consumed); released=consumed; }
    }
    v.release_z(released,v.size[2]);
    for(auto& plane:planes) {
        auto m=erode(plane,radius,rz);
        for(size_t i=0;i<votes.size();++i) votes[i]+=m.pixels[i];
    }
    planes.clear();
    int required=static_cast<int>(ps.size()/2+1);
    for(size_t i=0;i<votes.size();++i) fused.pixels[i]=votes[i]>=required;
    Mask raw=fused;
    retain_beam(fused,static_cast<int>(std::floor(c.gap/(v.spacing[2]*c.z_step))),static_cast<int>(std::floor(c.plate_thickness/(v.spacing[2]*c.z_step))));
    auto obstacles=c.end_guard?detached(std::move(raw),fused,band_lo,c.step,c.z_step,radius,rz,v,lateral_axis,c):std::vector<Obstacle>{};
    std::vector<int> profile(height);
    for(int z=0;z<height;++z) for(int u=0;u<width;++u) profile[z]+=fused.at(u,z);
    // Use the 90th percentile instead of a single potentially bright plate/streak.
    auto sorted=profile; std::sort(sorted.begin(),sorted.end());
    int peak=sorted[static_cast<size_t>(0.9*(height-1))];
    if(!peak) peak=*std::max_element(profile.begin(),profile.end());
    int cutoff=std::max(2,static_cast<int>(std::ceil(peak*0.2)));
    std::vector<uint8_t> occupied(height);
    for(int z=0;z<height;++z) occupied[z]=profile[z]>=cutoff;
    bridge(occupied,static_cast<int>(std::floor(c.gap/(v.spacing[2]*c.z_step))));
    auto r=runs(occupied);
    if(r.empty()) throw std::runtime_error("No longitudinal foreground; lower threshold/erosion or use smaller strides");
    Run longest=*std::max_element(r.begin(),r.end(),[](Run a,Run b){return a.second-a.first<b.second-b.first;});
    if((longest.second-longest.first)*c.z_step*v.spacing[2]<c.min_length)
        throw std::runtime_error("Candidate is shorter than --min-length-mm (possible holder or noise)");
    std::vector<int> lows,highs;
    for(int z=longest.first;z<longest.second;++z) if(profile[z]>=cutoff) {
        int lo=width,hi=0;
        for(int u=0;u<width;++u) if(fused.at(u,z)) { lo=std::min(lo,u); hi=std::max(hi,u+1); }
        if(hi>lo) { lows.push_back(lo); highs.push_back(hi); }
    }
    std::sort(lows.begin(),lows.end()); std::sort(highs.begin(),highs.end());
    // Reject isolated extreme rows, while retaining slow beam drift.
    size_t tail=lows.size()/200;
    int lo=lows[tail],hi=highs[highs.size()-1-tail];
    int first=0,last=height;
    while(first<height && !profile[first]) ++first;
    while(last>first && !profile[last-1]) --last;
    return {std::max(band_lo,band_lo+(lo-radius)*c.step),
            std::min(band_hi,band_lo+(hi+radius)*c.step),
            std::max(0,(first-rz)*c.z_step),
            std::min(v.size[2],(last+rz)*c.z_step),std::move(obstacles)};
}
std::vector<Box> detect(const Volume& v,const Config& c) {
    auto s=seeds(v,c);
    std::cerr << "Seed Y bands: " << s.size() << '\n';
    if(s.empty()) throw std::runtime_error("No Y bands detected; check threshold, spacing and minimum width");
    if(c.count && c.count!=static_cast<int>(s.size()))
        throw std::runtime_error("Expected " + std::to_string(c.count) + " beams but detected " + std::to_string(s.size()) + " Y bands; inspect threshold/erosion or use --box");
    std::vector<Box> boxes;
    for(size_t i=0;i<s.size();++i) {
        // Mid-gap partitions allow beam drift, but keep adjacent crops disjoint in Y.
        int band_lo=i?(s[i-1].y1+s[i].y0)/2:0;
        int band_hi=i+1<s.size()?(s[i].y1+s[i+1].y0)/2:v.size[1];
        try {
            auto yz=measure(v,c,1,band_lo,band_hi,s[i].x0,s[i].x1);
            auto xz=measure(v,c,0,0,v.size[0],s[i].y0,s[i].y1);
            Box b;
            int zm=px(c.z_margin,v.spacing[2]),zt=px(c.end_trim,v.spacing[2]);
            b.lo={std::max(0,xz.lateral_lo-px(c.margin,v.spacing[0])),std::max(band_lo,yz.lateral_lo-px(c.margin,v.spacing[1])),std::min(yz.zlo,xz.zlo)-zm+zt};
            b.hi={std::min(v.size[0],xz.lateral_hi+px(c.margin,v.spacing[0])),std::min(band_hi,yz.lateral_hi+px(c.margin,v.spacing[1])),std::max(yz.zhi,xz.zhi)+zm-zt};
            b.lo[2]=std::max(0,b.lo[2]); b.hi[2]=std::min(v.size[2],b.hi[2]);
            int original_lo=b.lo[2],original_hi=b.hi[2];
            int end_zone=std::max(1,(original_hi-original_lo)/10);
            auto guard=[&](const Measurement& measurement,int axis) {
                for(const auto& o:measurement.obstacles) {
                    if(o.hi<=b.lo[axis] || o.lo>=b.hi[axis] || o.zhi<=original_lo || o.zlo>=original_hi) continue;
                    // Compact detached blocks are plausible holders. Sparse
                    // streaks must not cause large end cuts.
                    if(o.fill<0.8) continue;
                    if(o.zhi<=original_lo+end_zone) b.lo[2]=std::max(b.lo[2],o.zhi);
                    else if(o.zlo>=original_hi-end_zone) b.hi[2]=std::min(b.hi[2],o.zlo);
                }
            };
            guard(yz,1); guard(xz,0);
            if(b.lo[2]!=original_lo || b.hi[2]!=original_hi)
                std::cerr<<"End guard, Y band "<<i+1<<": ["<<original_lo<<','<<original_hi<<") -> ["<<b.lo[2]<<','<<b.hi[2]<<")\n";
            if(b.hi[2]<=b.lo[2] || (b.hi[2]-b.lo[2])*v.spacing[2]<c.min_length) throw std::runtime_error("End trim removes the beam");
            boxes.push_back(b);
        } catch(const std::exception& e) {
            if(c.count) throw;
            std::cerr << "Skipping Y band ["<<s[i].y0<<','<<s[i].y1<<"): "<<e.what()<<'\n';
        }
    }
    if(boxes.empty()) throw std::runtime_error("No valid beams detected");
    std::sort(boxes.begin(),boxes.end(),[](const Box& a,const Box& b){return a.lo[1]+a.hi[1]>b.lo[1]+b.hi[1];});
    return boxes;
}
std::string json_string(const std::string& s) {
    std::ostringstream out; out<<'"';
    for(unsigned char c:s) {
        if(c=='"' || c=='\\') out<<'\\'<<c;
        else if(c<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;
        else out<<c;
    }
    out<<'"'; return out.str();
}
template<class T> std::string array_text(const std::array<T,3>& a) {
    std::ostringstream out; out<<std::setprecision(17)<<'['<<a[0]<<", "<<a[1]<<", "<<a[2]<<']'; return out.str();
}
int json_integer(const simple_json::Value& value,const std::string& field) {
    if(value.type!=simple_json::Value::Type::Number || !std::isfinite(value.number) ||
       value.number!=std::floor(value.number) || value.number<std::numeric_limits<int>::min() ||
       value.number>std::numeric_limits<int>::max())
        throw std::runtime_error("boxes.json field "+field+" must be an integer");
    return static_cast<int>(value.number);
}
I3 json_i3(const simple_json::Value& value,const std::string& field) {
    if(value.type!=simple_json::Value::Type::Array || value.array.size()!=3)
        throw std::runtime_error("boxes.json field "+field+" must contain three integers");
    I3 out;for(int d=0;d<3;++d) out[d]=json_integer(value.array[d],field);return out;
}
std::string json_text(const simple_json::Value& value,const std::string& field) {
    if(value.type!=simple_json::Value::Type::String)
        throw std::runtime_error("boxes.json field "+field+" must be a string");
    return value.string;
}
struct LoadedManifest { std::vector<Box> boxes; simple_json::Value document; double preview_threshold; };
LoadedManifest load_manifest(const fs::path& path,const Volume& v,const Config& c) {
    auto root=simple_json::parse_file(path);
    if(root.type!=simple_json::Value::Type::Object) throw std::runtime_error("boxes.json root must be an object");
    if(json_integer(root.at("schema_version"),"schema_version")!=1) throw std::runtime_error("Unsupported boxes.json schema_version");
    if(json_i3(root.at("source_sizes"),"source_sizes")!=v.size) throw std::runtime_error("boxes.json source dimensions differ from the input scan");
    auto expected_type=v.signed_type?"int16":"uint16";
    if(json_text(root.at("source_type"),"source_type")!=expected_type) throw std::runtime_error("boxes.json source type differs from the input scan");
    const auto& bytes=root.at("source_file_bytes");
    if(bytes.type!=simple_json::Value::Type::Number || bytes.number<0 ||
       bytes.number>static_cast<double>(std::numeric_limits<uintmax_t>::max()) || bytes.number!=std::floor(bytes.number) ||
       static_cast<uintmax_t>(bytes.number)!=fs::file_size(c.input))
        throw std::runtime_error("boxes.json source file size differs from the input scan");
    if(json_text(root.at("original_header"),"original_header")!=v.original_header)
        throw std::runtime_error("boxes.json source header differs from the input scan");
    auto& objects=root.at("objects");
    if(objects.type!=simple_json::Value::Type::Array || objects.array.empty()) throw std::runtime_error("boxes.json objects must be a non-empty array");
    std::vector<Box> boxes;boxes.reserve(objects.array.size());
    for(size_t i=0;i<objects.array.size();++i) {
        auto& object=objects.array[i];
        if(object.type!=simple_json::Value::Type::Object) throw std::runtime_error("boxes.json object "+std::to_string(i+1)+" must be an object");
        if(auto index=object.find("index");index && json_integer(*index,"objects.index")!=static_cast<int>(i+1))
            throw std::runtime_error("boxes.json object indices must be sequential and match array order");
        std::string file=json_text(object.at("file"),"objects.file");
        if(file.size()<=5 || file.substr(file.size()-5)!=".nrrd") throw std::runtime_error("boxes.json crop filenames must end in .nrrd");
        Box box;box.lo=json_i3(object.at("min"),"objects.min");box.hi=json_i3(object.at("max"),"objects.max");
        box.name=safe_name(file.substr(0,file.size()-5));
        I3 extent;for(int d=0;d<3;++d) extent[d]=box.hi[d]-box.lo[d];
        object.set("size",simple_json::Value::array_value({simple_json::Value::number_value(extent[0]),simple_json::Value::number_value(extent[1]),simple_json::Value::number_value(extent[2])}));
        boxes.push_back(std::move(box));
    }
    root.set("crops_written",simple_json::Value::boolean_value(false));
    auto parameters=root.find("parameters");
    if(!parameters) {root.set("parameters",simple_json::Value::object_value({}));parameters=root.find("parameters");}
    if(parameters->type!=simple_json::Value::Type::Object) throw std::runtime_error("boxes.json parameters must be an object");
    double preview_threshold=c.threshold;
    if(auto threshold=parameters->find("threshold")) {
        if(threshold->type!=simple_json::Value::Type::Number || !std::isfinite(threshold->number))
            throw std::runtime_error("boxes.json parameters.threshold must be a finite number");
        preview_threshold=threshold->number;
    }
    parameters->set("manifest_replay",simple_json::Value::boolean_value(true));
    parameters->set("box_preview",simple_json::Value::boolean_value(c.box_preview));
    return {std::move(boxes),std::move(root),preview_threshold};
}
std::string manifest(const Volume& v,const Config& c,const std::vector<Box>& boxes) {
    std::ostringstream out; out<<std::setprecision(17);
    out<<"{\n  \"schema_version\": 1,\n  \"source\": "<<json_string(fs::absolute(c.input).string())
       <<",\n  \"source_sizes\": "<<array_text(v.size)<<",\n  \"source_type\": "<<json_string(v.signed_type?"int16":"uint16")
       <<",\n  \"source_file_bytes\": "<<fs::file_size(c.input)<<",\n  \"spacing_mm\": "<<array_text(v.spacing)
       <<",\n  \"original_header\": "<<json_string(v.original_header)
       <<",\n  \"axis_order\": [\"x\", \"y\", \"z\"],\n  \"object_order\": \"decreasing voxel Y\",\n  \"bounds\": \"min inclusive, max exclusive\",\n  \"crops_written\": "<<(c.preview?"false":"true")
       <<",\n  \"parameters\": {\"threshold\": "<<c.threshold<<", \"metal_threshold\": "<<c.metal_threshold<<", \"erosion_mm\": "<<c.erosion
       <<", \"margin_mm\": "<<c.margin<<", \"z_margin_mm\": "<<c.z_margin<<", \"end_trim_mm\": "<<c.end_trim<<", \"gap_mm\": "<<c.gap<<", \"plate_thickness_mm\": "<<c.plate_thickness
       <<", \"min_width_mm\": "<<c.min_width<<", \"min_length_mm\": "<<c.min_length<<", \"samples\": "<<c.samples
       <<", \"cross_sections\": "<<c.cross_sections<<", \"step\": "<<c.step<<", \"z_step\": "<<c.z_step
       <<", \"box_preview\": "<<(c.box_preview?"true":"false")<<", \"manifest_replay\": false, \"end_guard\": "<<(c.end_guard?"true":"false")<<", \"explicit_boxes\": "<<(c.boxes.empty()?"false":"true")<<"},\n  \"objects\": [\n";
    for(size_t i=0;i<boxes.size();++i) {
        const auto& b=boxes[i]; I3 extent; for(int d=0;d<3;++d) extent[d]=b.hi[d]-b.lo[d];
        out<<"    {\"index\": "<<i+1<<", \"file\": "<<json_string(b.name+".nrrd")<<", \"min\": "<<array_text(b.lo)<<", \"max\": "<<array_text(b.hi)<<", \"size\": "<<array_text(extent)<<'}'<<(i+1<boxes.size()?",":"")<<'\n';
    }
    out<<"  ]\n}\n"; return out.str();
}
void write_crops(const Volume& v,const Config& c,const std::vector<Box>& boxes) {
    std::vector<std::ofstream> files; std::vector<fs::path> temporaries;
    try {
        for(const auto& b:boxes) {
            auto path=c.output/(b.name+".nrrd.partial"); temporaries.push_back(path);
            files.emplace_back(path,std::ios::binary); if(!files.back()) throw std::runtime_error("Cannot create crop " + path.string());
            files.back()<<v.header(b);
        }
        // One sequential Z traversal for all crops; never allocate a 3D image.
        int zlo=v.size[2],zhi=0;
        for(const auto& b:boxes) { zlo=std::min(zlo,b.lo[2]); zhi=std::max(zhi,b.hi[2]); }
        int last_percent=-1,released=zlo;
        for(int z=zlo;z<zhi;++z) {
            for(size_t i=0;i<boxes.size();++i) {
                const auto& b=boxes[i]; if(z<b.lo[2] || z>=b.hi[2]) continue;
                for(int y=b.lo[1];y<b.hi[1];++y) {
                    auto p=v.data+2*v.index(b.lo[0],y,z);
                    files[i].write(reinterpret_cast<const char*>(p),2*static_cast<std::streamsize>(b.hi[0]-b.lo[0]));
                }
                if(!files[i]) throw std::runtime_error("Crop write failed (disk full or I/O error)");
            }
            if(z+1-released>=64) { v.release_z(released,z+1); released=z+1; }
            int percent=(z-zlo+1)*100/(zhi-zlo);
            if(percent/10!=last_percent/10) { std::cerr<<"Writing crops: "<<percent<<"%\n"; last_percent=percent; }
        }
        v.release_z(released,zhi);
        for(auto& file:files) { file.close(); if(!file) throw std::runtime_error("Crop flush failed"); }
        for(size_t i=0;i<boxes.size();++i) fs::rename(temporaries[i],c.output/(boxes[i].name+".nrrd"));
    } catch(...) {
        for(auto& file:files) file.close();
        for(auto& path:temporaries) { std::error_code ec; fs::remove(path,ec); }
        throw;
    }
}
int main(int argc,char** argv) {
    try {
        auto start=std::chrono::steady_clock::now();
        Config c=parse(argc,argv); Volume v(c.input);
        std::cerr<<"Scan "<<array_text(v.size)<<", "<<(v.signed_type?"int16":"uint16")<<", spacing "<<array_text(v.spacing)<<" mm\n";
        auto json_path=c.output/"boxes.json",partial=c.output/"boxes.json.partial";
        bool reuse_manifest=fs::exists(json_path);
        if(reuse_manifest) {
            if(!fs::is_regular_file(json_path)) throw std::runtime_error("Output boxes.json is not a regular file");
            for(const auto& entry:fs::directory_iterator(c.output)) {
                if(lower(entry.path().extension().string())==".nrrd")
                    throw std::runtime_error("Output contains NRRD crops. Delete all crop NRRDs before replaying edited boxes.json");
                if(entry.path().extension()==".partial") throw std::runtime_error("Output contains an unfinished .partial file; resolve it before replay");
            }
            if(!c.box_preview) {
                c.box_preview=true;
                std::cerr<<"Manifest replay always regenerates boxes_preview.png; ignoring --no-box-preview.\n";
            }
            std::cerr<<"Reusing edited boxes from "<<json_path<<"; segmentation and margin options are skipped.\n";
        }
        if(fs::exists(partial)) throw std::runtime_error("Unfinished boxes.json.partial already exists");
        simple_json::Value reused_document;
        std::vector<Box> boxes;
        if(reuse_manifest) {
            auto loaded=load_manifest(json_path,v,c);boxes=std::move(loaded.boxes);reused_document=std::move(loaded.document);c.threshold=loaded.preview_threshold;
        } else boxes=c.boxes.empty()?detect(v,c):c.boxes;
        if(!reuse_manifest && c.count && c.count!=static_cast<int>(boxes.size())) throw std::runtime_error("Count and boxes disagree");
        std::set<std::string> names;
        for(size_t i=0;i<boxes.size();++i) {
            auto& b=boxes[i];
            for(int d=0;d<3;++d) if(b.lo[d]<0 || b.lo[d]>=b.hi[d] || b.hi[d]>v.size[d])
                throw std::runtime_error("Box "+std::to_string(i+1)+" is empty or outside the source scan");
            if(!reuse_manifest) b.name=safe_name(c.labels.empty()?c.input.stem().string()+"_"+std::to_string(i+1):c.labels[i]);
            if(!names.insert(b.name).second) throw std::runtime_error("Duplicate output name: "+b.name);
            if(fs::exists(c.output/(b.name+".nrrd")) || fs::exists(c.output/(b.name+".nrrd.partial"))) throw std::runtime_error("Output already exists: "+b.name);
            std::cerr<<i+1<<": "<<b.name<<" "<<array_text(b.lo)<<" -> "<<array_text(b.hi)<<'\n';
        }
        fs::create_directories(c.output);
        if(!reuse_manifest && fs::exists(json_path)) throw std::runtime_error("boxes.json already exists; choose a new output directory");
        auto image_path=c.output/"boxes_preview.png",image_partial=c.output/"boxes_preview.png.partial";
        if(fs::exists(image_partial)) throw std::runtime_error("Unfinished boxes_preview.png.partial already exists");
        if(!reuse_manifest && c.box_preview && fs::exists(image_path)) throw std::runtime_error("Box preview already exists; choose a new output directory");
        for(const auto& b:boxes) v.header(b);
        auto publish=[&](bool crops_written) {
            std::string text;
            if(reuse_manifest) {
                reused_document.set("crops_written",simple_json::Value::boolean_value(crops_written));
                text=simple_json::dump(reused_document,2);
            } else {
                Config state=c;state.preview=!crops_written;text=manifest(v,state,boxes);
            }
            std::ofstream out(partial);out<<text;out.close();
            if(!out) {std::error_code ec;fs::remove(partial,ec);throw std::runtime_error("Cannot write manifest");}
            fs::rename(partial,json_path);
        };
        publish(false);
        if(c.box_preview) {
            std::vector<PreviewBox> preview_boxes;
            for(const auto& b:boxes) preview_boxes.push_back({b.lo,b.hi,b.name});
            try {
                write_box_preview(image_partial,v.size,preview_boxes,[&](int x,int y,int z){return v.value(x,y,z);},[&](){v.release_z(0,v.size[2]);},c.threshold);
                fs::rename(image_partial,image_path);
            } catch(...) {std::error_code ec;fs::remove(image_partial,ec);throw;}
            std::cerr<<"Box image: "<<image_path<<'\n';
        }
        if(!c.preview) {write_crops(v,c,boxes);publish(true);}
        std::cerr<<(c.preview?"Preview":"Done")<<": "<<boxes.size()<<" beams, "<<json_path<<", "
                 <<std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()<<" s\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"Error: "<<e.what()<<'\n';return 1;}
}
