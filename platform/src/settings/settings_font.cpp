#include "port/settings_font.h"
#include <algorithm>
#include <cstring>

namespace port::settings {
namespace {
struct Reader {
    const uint8_t* bytes;size_t size;bool le;
    bool range(uint64_t at,uint64_t count) const {return at<=size&&count<=size-at;}
    uint16_t u16(size_t at) const {return static_cast<uint16_t>(le?bytes[at]|(bytes[at+1]<<8):(bytes[at]<<8)|bytes[at+1]);}
    uint32_t u32(size_t at) const {uint32_t v=0;for(unsigned i=0;i<4;++i)v|=static_cast<uint32_t>(bytes[at+i])<<(8*(le?i:3-i));return v;}
    bool tag(size_t at,const char* name) const {for(unsigned i=0;i<4;++i)if(bytes[at+i]!=static_cast<uint8_t>(name[le?3-i:i]))return false;return true;}
};
enum class Kind { Info, Texture, Map, Width, Glyph, Unknown };
struct Block {size_t at=0,size=0;Kind kind=Kind::Unknown;};
bool Fail(const char*& error,const char* message) {error=message;return false;}
struct Budget {
    const FontAllocator& allocator;
    size_t remaining=kFontDecodeLimit;
    bool exhausted=false;
    template<class T> bool Allocate(FontBuffer<T>& out,size_t count,const char*& error) {
        if(count>remaining/sizeof(T)||!out.Allocate(count,allocator)) {
            exhausted=true;return Fail(error,"Font memory unavailable or combined decode budget exceeded");
        }
        remaining-=count*sizeof(T);return true;
    }
};
size_t GlyphPixel(const GameFont& f,const GameGlyph& g,unsigned x,unsigned y) {
    if(x>=static_cast<unsigned>(g.width)||y>=f.cellHeight)return f.alpha.size();
    return (static_cast<size_t>(g.sheet)*f.sheetHeight+g.y+y)*f.sheetWidth+g.x+x;
}
bool LinksValid(const Reader& r,const Block* blocks,size_t count,Kind kind,const char*& error) {
    int next[1024];std::fill(std::begin(next),std::end(next),-1);
    for(size_t i=0;i<count;++i)if(blocks[i].kind==kind) {
        const size_t link=r.u32(blocks[i].at+(kind==Kind::Map?16:12));
        if(!link)continue;
        for(size_t j=0;j<count;++j)if(blocks[j].kind==kind&&blocks[j].at+8==link){next[i]=static_cast<int>(j);break;}
        if(next[i]<0)return Fail(error,"Invalid font block link");
    }
    for(size_t i=0;i<count;++i)if(blocks[i].kind==kind) {
        int at=static_cast<int>(i);size_t steps=0;
        while(at>=0) {if(++steps>count)return Fail(error,"Cyclic font block link");at=next[at];}
    }
    return true;
}
}
bool GameFont::valid() const {return cellHeight&&nominalHeight&&sheetWidth&&sheetHeight&&!glyphs.empty()&&alternate<glyphs.size()&&codepoints.size()==65536&&!alpha.empty();}
const GameGlyph* GameFont::find(uint32_t codepoint,bool useAlternate) const {
    if(!valid())return nullptr;
    int32_t index=codepoint<codepoints.size()?codepoints[codepoint]:-1;
    if(index<0) {if(!useAlternate)return nullptr;index=alternate;}
    return static_cast<size_t>(index)<glyphs.size()?&glyphs[static_cast<size_t>(index)]:nullptr;
}
uint8_t GameFont::sample(const GameGlyph& g,unsigned x,unsigned y) const {const auto index=GlyphPixel(*this,g,x,y);return index<alpha.size()?alpha[index]:0;}
uint8_t GameFont::shade(const GameGlyph& g,unsigned x,unsigned y) const {if(luminance.empty())return 255;const auto index=GlyphPixel(*this,g,x,y);return index<luminance.size()?luminance[index]:0;}
static bool ParseFont(const void* data,size_t bytes,GameFont& out,const char*& error,Budget& budget) {
    out={};error=nullptr;
    if(!data||bytes<16||bytes>kFontFileLimit)return Fail(error,"Font size outside bounds");
    const auto* p=static_cast<const uint8_t*>(data);
    bool le=std::memcmp(p,"TNFR",4)==0;
    if(!le&&std::memcmp(p,"RFNT",4)!=0)return Fail(error,"Not an unmodified RFNT font");
    Reader r{p,bytes,le};
    if(r.u16(4)!=0xfeff||(r.u16(6)!=0x104&&r.u16(6)!=0x102))return Fail(error,"Unsupported font byte order/version");
    const size_t declared=r.u32(8),header=r.u16(12),count=r.u16(14);
    if(declared>bytes||declared<16||header<16||header>declared||count<2||count>1024)return Fail(error,"Invalid font header");
    r.size=declared;Block blocks[1024];size_t at=header;const Block* info=nullptr;const Block* texture=nullptr;
    for(size_t i=0;i<count;++i) {
        if(!r.range(at,8))return Fail(error,"Truncated font block");
        const size_t size=r.u32(at+4);if(size<8||!r.range(at,size))return Fail(error,"Invalid font block size");
        const Kind kind=r.tag(at,"FINF")?Kind::Info:r.tag(at,"TGLP")?Kind::Texture:r.tag(at,"CMAP")?Kind::Map:r.tag(at,"CWDH")?Kind::Width:r.tag(at,"GLGR")?Kind::Glyph:Kind::Unknown;
        blocks[i]={at,size,kind};at+=size;
    }
    for(size_t i=0;i<count;++i) {
        const auto& b=blocks[i];
        if(b.kind==Kind::Info) {if(info||b.size<32)return Fail(error,"Invalid font information");info=&b;}
        if(b.kind==Kind::Texture) {if(texture||b.size<32)return Fail(error,"Invalid font texture");texture=&b;}
        if((b.kind==Kind::Map&&b.size<20)||(b.kind==Kind::Width&&b.size<16))return Fail(error,"Truncated font tables");
        if(b.kind==Kind::Unknown)return Fail(error,"Unsupported font block");
    }
    if(!info||!texture)return Fail(error,"Missing font information/texture");
    if(!LinksValid(r,blocks,count,Kind::Map,error)||!LinksValid(r,blocks,count,Kind::Width,error))return false;
    auto pointer=[&](size_t value,Kind type) {if(!value)return true;for(size_t i=0;i<count;++i)if(blocks[i].at+8==value&&blocks[i].kind==type)return true;return false;};
    const size_t fi=info->at+8,tx=texture->at+8;
    if(r.u32(fi+8)!=tx||!pointer(r.u32(fi+12),Kind::Width)||!r.u32(fi+16)||!pointer(r.u32(fi+16),Kind::Map)||p[fi+7]>1)return Fail(error,"Invalid font resource references/encoding");
    const unsigned cw=p[tx],ch=p[tx+1],sheetCount=r.u16(tx+8),format=r.u16(tx+10),columns=r.u16(tx+12),rows=r.u16(tx+14),sw=r.u16(tx+16),sh=r.u16(tx+18);
    const uint64_t sheetSize=r.u32(tx+4),image=r.u32(tx+20),pixels=static_cast<uint64_t>(sw)*sh*sheetCount,glyphCount=static_cast<uint64_t>(columns)*rows*sheetCount;
    const unsigned tw=format==3?4:8,th=format==0?8:4,bpp=format==0?0:format==3?2:1;
    if(format>3||!cw||!ch||!sheetCount||sheetCount>64||!columns||!rows||!sw||!sh||sw>2048||sh>2048||sw%tw||sh%th||static_cast<uint64_t>(columns)*(cw+1)>sw||static_cast<uint64_t>(rows)*(ch+1)>sh||!glyphCount||glyphCount>65535||!pixels||pixels>kFontAlphaLimit/(format>=2?2:1))return Fail(error,"Invalid or oversized font sheet geometry");
    const uint64_t expected=format==0?static_cast<uint64_t>(sw)*sh/2:static_cast<uint64_t>(sw)*sh*bpp;
    if(sheetSize!=expected||image<tx+24||image>texture->at+texture->size||sheetSize*sheetCount>texture->at+texture->size-image||!r.range(image,sheetSize*sheetCount))return Fail(error,"Invalid font sheet data");
    GameFont f;f.cellHeight=ch;f.nominalHeight=p[fi+20];f.ascent=p[fi+22];f.sheetWidth=sw;f.sheetHeight=sh;f.alternate=r.u16(fi+2);
    if(!f.nominalHeight||f.nominalHeight>128||f.ascent>128||f.alternate>=glyphCount)return Fail(error,"Invalid font metrics");
    const uint64_t decoded=pixels*(format>=2?2:1)+glyphCount*sizeof(GameGlyph)+65536*sizeof(int32_t);
    if(decoded>budget.remaining){budget.exhausted=true;return Fail(error,"Combined font decode budget exceeded");}
    if(!budget.Allocate(f.alpha,static_cast<size_t>(pixels),error)||
       (format>=2&&!budget.Allocate(f.luminance,static_cast<size_t>(pixels),error))||
       !budget.Allocate(f.glyphs,static_cast<size_t>(glyphCount),error)||
       !budget.Allocate(f.codepoints,65536,error))return false;
    std::fill(f.codepoints.begin(),f.codepoints.end(),-1);
    size_t source=static_cast<size_t>(image);
    for(unsigned sheet=0;sheet<sheetCount;++sheet)for(unsigned ty=0;ty<sh;ty+=th)for(unsigned tx0=0;tx0<sw;tx0+=tw)for(unsigned y=0;y<th;++y)for(unsigned x=0;x<tw;++x) {
        uint8_t alpha,shade=255;
        if(format==0) {alpha=static_cast<uint8_t>(((p[source]>>(x%2?0:4))&15)*17);if(x%2)++source;}
        else if(format==1)alpha=p[source++];
        else if(format==2) {alpha=static_cast<uint8_t>((p[source]>>4)*17);shade=static_cast<uint8_t>((p[source++]&15)*17);}
        else {alpha=p[source++];shade=p[source++];}
        const size_t index=(static_cast<size_t>(sheet)*sh+ty+y)*sw+tx0+x;f.alpha[index]=alpha;if(!f.luminance.empty())f.luminance[index]=shade;
    }
    const int left=static_cast<int8_t>(p[fi+4]),width=p[fi+5],advance=static_cast<int8_t>(p[fi+6]);
    auto metrics=[&](GameGlyph& glyph,int bearing,int ink,int spacing) {
        if(bearing<-static_cast<int>(cw)||bearing>static_cast<int>(cw)||ink>static_cast<int>(cw+1)||spacing<0)return false;
        glyph.left=static_cast<int16_t>(bearing);glyph.width=static_cast<int16_t>(ink);glyph.advance=static_cast<int16_t>(spacing);return static_cast<unsigned>(glyph.x+ink)<=sw;
    };
    for(size_t i=0;i<f.glyphs.size();++i) {
        auto& g=f.glyphs[i];const unsigned cell=static_cast<unsigned>(i)%(columns*rows);g.sheet=static_cast<uint16_t>(i/(columns*rows));g.x=static_cast<uint16_t>((cell%columns)*(cw+1)+1);g.y=static_cast<uint16_t>((cell/columns)*(ch+1)+1);
        if(!metrics(g,left,width,advance))return Fail(error,"Invalid default font width");
    }
    for(size_t blockIndex=0;blockIndex<count;++blockIndex)if(blocks[blockIndex].kind==Kind::Width) {
        const auto& block=blocks[blockIndex];
        size_t d=block.at+8;unsigned first=r.u16(d),last=r.u16(d+2);
        if(first>last||last>=glyphCount||8+static_cast<uint64_t>(last-first+1)*3>block.size-8)return Fail(error,"Invalid font width range");
        for(unsigned index=first;index<=last;++index) {size_t w=d+8+(index-first)*3;if(!metrics(f.glyphs[index],static_cast<int8_t>(p[w]),p[w+1],static_cast<int8_t>(p[w+2])))return Fail(error,"Invalid glyph width");}
    }
    auto map=[&](unsigned code,unsigned glyph) {if(glyph==0xffff)return true;if(glyph>=glyphCount)return false;auto& to=f.codepoints[code];if(to>=0&&to!=static_cast<int32_t>(glyph))return false;to=static_cast<int32_t>(glyph);return true;};
    for(size_t blockIndex=0;blockIndex<count;++blockIndex)if(blocks[blockIndex].kind==Kind::Map) {
        const auto& block=blocks[blockIndex];
        size_t d=block.at+8;unsigned first=r.u16(d),last=r.u16(d+2),method=r.u16(d+4);if(first>last||method>2)return Fail(error,"Invalid character map");
        const uint64_t entries=last-first+1;
        if(method==0) {if(block.size<22)return Fail(error,"Truncated direct character map");unsigned start=r.u16(d+12);for(unsigned c=first;c<=last;++c)if(!map(c,start+c-first))return Fail(error,"Invalid direct glyph index");}
        else if(method==1) {if(12+entries*2>block.size-8)return Fail(error,"Truncated table character map");for(unsigned c=first;c<=last;++c)if(!map(c,r.u16(d+12+(c-first)*2)))return Fail(error,"Invalid table glyph index");}
        else {if(block.size<22)return Fail(error,"Truncated scanned character map");unsigned n=r.u16(d+12);if(14+static_cast<uint64_t>(n)*4>block.size-8)return Fail(error,"Truncated scanned map entries");for(unsigned i=0;i<n;++i) {unsigned code=r.u16(d+14+i*4),glyph=r.u16(d+16+i*4);if(code<first||code>last||!map(code,glyph))return Fail(error,"Invalid scanned glyph index");}}
    }
    if(std::none_of(f.codepoints.begin(),f.codepoints.end(),[](int32_t glyph){return glyph>=0;}))return Fail(error,"Empty character map");
    out=std::move(f);return true;
}
bool ParseGameFont(const void* data,size_t bytes,GameFont& out,const char*& error,const FontAllocator& allocator) {
    Budget budget{allocator};return ParseFont(data,bytes,out,error,budget);
}
bool ParseGameFontArchive(const void* data,size_t bytes,FontBundle& out,const char*& error,const FontAllocator& allocator) {
    out={};error=nullptr;if(!data||bytes<64||bytes>kFontArchiveLimit)return Fail(error,"Font archive size outside bounds");
    const auto* p=static_cast<const uint8_t*>(data);bool le=std::memcmp(p,"CRAR",4)==0;if(!le&&std::memcmp(p,"RARC",4)!=0)return Fail(error,"Not an uncompressed font archive");
    Reader r{p,bytes,le};const uint64_t declared=r.u32(4),header=r.u32(8),image=header+r.u32(12);
    if(declared>bytes||declared<64||header<32||!r.range(header,32))return Fail(error,"Invalid font archive header");r.size=static_cast<size_t>(declared);
    const uint64_t dirs=r.u32(static_cast<size_t>(header)),dirAt=header+r.u32(static_cast<size_t>(header)+4),files=r.u32(static_cast<size_t>(header)+8),fileAt=header+r.u32(static_cast<size_t>(header)+12),stringBytes=r.u32(static_cast<size_t>(header)+16),strings=header+r.u32(static_cast<size_t>(header)+20);
    if(!dirs||dirs>1024||files>16384||!r.range(dirAt,dirs*16)||!r.range(fileAt,files*20)||!stringBytes||!r.range(strings,stringBytes)||image>r.size)return Fail(error,"Invalid font archive tables");
    auto name=[&](uint64_t offset,char (&value)[256]) {
        if(offset>=stringBytes)return false;const char* start=reinterpret_cast<const char*>(p+strings+offset);const auto* end=static_cast<const char*>(std::memchr(start,0,static_cast<size_t>(stringBytes-offset)));if(!end||end-start>255)return false;
        const size_t length=static_cast<size_t>(end-start);
        for(size_t i=0;i<length;++i)value[i]=start[i]>='A'&&start[i]<='Z'?static_cast<char>(start[i]-'A'+'a'):start[i];
        value[length]=0;return true;
    };
    char filename[256];
    for(unsigned i=0;i<dirs;++i) {
        size_t d=static_cast<size_t>(dirAt)+i*16;uint64_t first=r.u32(d+12),count=r.u16(d+10);
        if(first>files||count>files-first||!name(r.u32(d+4),filename))return Fail(error,"Invalid font directory range");
    }
    // Bounded metadata lives on the stack: no STL allocation or recursive
    // callable can enter the target's fatal-new path for an optional font.
    uint8_t colors[1024]{};
    struct Frame {unsigned dir;uint64_t next,end;};Frame stack[65];
    auto frame=[&](unsigned dir) {const size_t d=static_cast<size_t>(dirAt)+dir*16;const uint64_t first=r.u32(d+12);return Frame{dir,first,first+r.u16(d+10)};};
    for(unsigned start=0;start<dirs;++start) {
        if(colors[start])continue;
        unsigned depth=0;stack[0]=frame(start);colors[start]=1;
        for(;;) {
            auto& current=stack[depth];
            if(current.next==current.end) {colors[current.dir]=2;if(!depth)break;--depth;continue;}
            const size_t e=static_cast<size_t>(fileAt+current.next++*20);const uint32_t word=r.u32(e+4);
            if(!((word>>24)&2))continue;
            if(!name(word&0xffffff,filename))return Fail(error,"Invalid font folder name");
            if(!std::strcmp(filename,".")||!std::strcmp(filename,".."))continue;
            const unsigned child=r.u32(e+8);
            if(child>=dirs)return Fail(error,"Invalid font folder index");
            if(colors[child]==1)return Fail(error,"Cyclic font archive folders");
            if(colors[child]==2)continue;
            if(depth==64)return Fail(error,"Font archive folder nesting exceeds limit");
            stack[++depth]=frame(child);colors[child]=1;
        }
    }
    struct Entry {uint64_t offset=0,size=0;bool seen=false;};Entry menu,message;
    for(unsigned i=0;i<files;++i) {
        size_t e=static_cast<size_t>(fileAt)+i*20;uint32_t word=r.u32(e+4);if(!name(word&0xffffff,filename))return Fail(error,"Invalid font archive filename");
        if((word>>24)&2)continue;
        uint64_t offset=image+r.u32(e+8),size=r.u32(e+12);if(!r.range(offset,size))return Fail(error,"Invalid font archive file range");
        Entry* target=!std::strcmp(filename,"menufont64.brfnt")?&menu:!std::strcmp(filename,"messagefont26.brfnt")?&message:nullptr;
        if(target) {if(target->seen)return Fail(error,"Ambiguous font archive entry");if((word>>24)&0x84)return Fail(error,"Compressed font entry is unavailable");*target={offset,size,true};}
    }
    FontBundle bundle;Budget budget{allocator};const char* fontError=nullptr;
    bool haveMenu=menu.size&&ParseFont(p+menu.offset,static_cast<size_t>(menu.size),bundle.menu,fontError,budget);
    if(budget.exhausted)return Fail(error,fontError);
    bool haveMessage=message.size&&ParseFont(p+message.offset,static_cast<size_t>(message.size),bundle.message,fontError,budget);
    if(budget.exhausted)return Fail(error,fontError);
    if(!haveMenu&&!haveMessage)return Fail(error,"No supported menu/message font in archive");
    if(!haveMenu)error="Menu font unavailable; message font retained";else if(!haveMessage)error="Message font unavailable; menu font retained";
    out=std::move(bundle);return true;
}
uint32_t NextFontCodepoint(const std::string& text,size_t& offset) {
    if(offset>=text.size())return 0;
    const size_t start=offset;unsigned first=static_cast<unsigned char>(text[offset++]);if(first<0x80)return first;
    unsigned count=first>=0xc2&&first<=0xdf?2:first>=0xe0&&first<=0xef?3:first>=0xf0&&first<=0xf4?4:0;if(!count||text.size()-start<count)return 0xfffd;
    uint32_t code=first&((1u<<(7-count))-1);
    for(unsigned i=1;i<count;++i) {unsigned next=static_cast<unsigned char>(text[start+i]);if((next&0xc0)!=0x80)return 0xfffd;code=(code<<6)|(next&0x3f);}
    if(code<(count==2?0x80u:count==3?0x800u:0x10000u)||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return 0xfffd;
    offset=start+count;return code;
}
}
