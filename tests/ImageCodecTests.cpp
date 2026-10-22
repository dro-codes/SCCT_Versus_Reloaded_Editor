#include "../Reloaded.Editor.Mcp/ImageCodec.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace Mcp;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Reject(F f,const std::string& expected={},std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception& e){caught=true;if(!expected.empty() && std::string(e.what()).find(expected)==std::string::npos)throw std::runtime_error("line "+std::to_string(where.line())+": wrong error: "+e.what());}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}
// Fixtures live in tests/mcp/fixtures; the suite runs from the repository
// root (tools/test_mcp.cmd) or from tests/.
std::string Fixture(const std::string& name)
{
    for(const auto& dir:{std::filesystem::path("tests/mcp/fixtures"),std::filesystem::path("mcp/fixtures"),std::filesystem::path(__FILE__).parent_path()/"mcp/fixtures"})
    {
        std::ifstream in(dir/name,std::ios::binary);
        if(in){std::ostringstream s;s<<in.rdbuf();return s.str();}
    }
    throw std::runtime_error("missing fixture "+name);
}
Image::Rgba Expected(const std::string& name)
{
    const auto raw=Fixture(name+".rgba");
    Check(raw.size()>=12 && raw.compare(0,4,"RGBA")==0,"bad .rgba fixture");
    auto u32=[&](size_t at){return static_cast<int>(static_cast<uint8_t>(raw[at])|(static_cast<uint8_t>(raw[at+1])<<8)|(static_cast<uint8_t>(raw[at+2])<<16)|(static_cast<uint8_t>(raw[at+3])<<24));};
    Image::Rgba image(u32(4),u32(8));
    Check(raw.size()==12+image.pixels.size(),"bad .rgba fixture size");
    std::memcpy(image.pixels.data(),raw.data()+12,image.pixels.size());
    return image;
}
bool Same(const Image::Rgba& a,const Image::Rgba& b){return a.width==b.width && a.height==b.height && a.pixels==b.pixels;}
Image::Rgba Pattern(int w,int h,bool alpha)
{
    Image::Rgba image(w,h);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)
    {
        auto* p=image.At(x,y);
        p[0]=static_cast<uint8_t>(x*9+y);p[1]=static_cast<uint8_t>(y*5);p[2]=static_cast<uint8_t>((x*y)&0xff);
        p[3]=alpha?static_cast<uint8_t>((x+y)*3):uint8_t{255};
    }
    return image;
}
int main()
{
    try
    {
        // Checksums against their published check values.
        Check(Image::Crc32("123456789",9)==0xCBF43926u,"crc32 check value");
        Check(Image::Adler32("Wikipedia",9)==0x11E60398u,"adler32 check value");
        // Split updates give the same checksum.
        Check(Image::Crc32("6789",4,Image::Crc32("12345",5))==0xCBF43926u,"crc32 continues");

        // Deflate round trips: stored, LZ77 on text, on a run, on noise, empty.
        std::string text;for(int i=0;i<500;++i)text+="the plan of room "+std::to_string(i%17)+" is a rectangle; ";
        std::string run(100000,'x'),noise;
        uint32_t seed=12345;for(int i=0;i<70000;++i){seed=seed*1103515245u+12345u;noise+=static_cast<char>(seed>>24);}
        for(const auto* data:{&text,&run,&noise})
            for(int level:{0,1,6,9})
            {
                const auto packed=Image::ZlibCompress(*data,level);
                Check(Image::ZlibDecompress(packed)==*data,"zlib round trip");
                if(level>0 && data!=&noise)Check(packed.size()<data->size()/4,"LZ77 compresses repetitive data");
            }
        Check(Image::ZlibDecompress(Image::ZlibCompress(std::string()))==std::string(),"empty round trip");
        Check(Image::ZlibCompress(run,6).size()<1200,"a long run costs a few bits per 258 bytes");
        Reject([&]{Image::ZlibDecompress(Image::ZlibCompress(text),100);},"larger than expected");
        {
            auto packed=Image::ZlibCompress(text);packed[packed.size()-1]^=1;
            Reject([&]{Image::ZlibDecompress(packed);},"checksum");
            Reject([&]{Image::ZlibDecompress(Image::ZlibCompress(text).substr(0,40));});
            Reject([&]{Image::ZlibDecompress("not zlib");},"not a zlib");
        }
        // A real gzip file (Python's gzip, dynamic Huffman).
        {
            const auto lines=Image::GzipDecompress(Fixture("lines.gz"));
            std::string expected;for(int i=0;i<200;++i)expected+="line "+std::to_string(i)+" of the gzip fixture\n";
            Check(lines==expected,"gzip fixture");
        }

        // Real PNGs written by Pillow (zlib, adaptive filters) decode exactly.
        for(const char* name:{"rgba8","rgb8","grey8","grey1","greya8","palette4","rgb16"})
        {
            const auto decoded=Image::DecodePng(Fixture(std::string(name)+".png"));
            if(!Same(decoded,Expected(name)))throw std::runtime_error(std::string("PNG fixture decodes differently: ")+name);
        }
        Check(Image::PngSize(Fixture("rgba8.png"))==std::make_pair(37,23),"png size");
        Reject([&]{Image::DecodePng(Fixture("interlaced.png"));},"Interlaced");
        Reject([&]{Image::DecodePng("hello");},"Not a PNG");
        {
            auto png=Fixture("rgba8.png");png[60]^=0x40;
            Reject([&]{Image::DecodePng(png);},"corrupt");
            Reject([&]{Image::DecodePng(Fixture("rgba8.png").substr(0,200));});
        }
        // Palette transparency survived as alpha 0.
        {
            const auto palette=Image::DecodePng(Fixture("palette4.png"));bool clear=false;
            for(size_t i=3;i<palette.pixels.size();i+=4)clear=clear||palette.pixels[i]==0;
            Check(clear,"tRNS palette entry is transparent");
        }

        // PNG encoding round trips through the decoder in every colour type.
        {
            const auto rgba=Pattern(53,31,true),rgb=Pattern(53,31,false);
            Image::Rgba flat(40,20,{10,200,30,255});
            for(int x=0;x<20;++x)for(int y=0;y<10;++y){auto* p=flat.At(x,y);p[0]=250;p[1]=250;p[2]=240;}
            Image::Rgba clearFlat=flat;clearFlat.At(3,3)[3]=0;
            for(const Image::Rgba* image:{&rgba,&rgb,static_cast<const Image::Rgba*>(&flat),static_cast<const Image::Rgba*>(&clearFlat)})
                for(int level:{0,6})
                    for(bool palette:{true,false})
                    {
                        Image::PngOptions options;options.level=level;options.palette=palette;
                        const auto png=Image::EncodePng(*image,options);
                        Check(Image::IsPng(png),"png signature");
                        Check(Same(Image::DecodePng(png),*image),"png round trip");
                    }
            // A flat image becomes a small palette PNG.
            const auto png=Image::EncodePng(flat);
            Check(png[25]==3,"few colours give a palette PNG");
            Check(png.size()<200,"flat palette PNG is small");
            Check(Image::EncodePng(rgb)[25]==2 && Image::EncodePng(rgba,{6,false,true})[25]==6,"colour type choice");
            // Without alpha the RGB values stay and alpha becomes opaque.
            auto dropped=Image::DecodePng(Image::EncodePng(rgba,{6,false,false}));
            Check(dropped.Opaque() && dropped.At(5,7)[0]==rgba.At(5,7)[0],"alpha dropped");
        }

        // BMP and TGA.
        Check(Same(Image::DecodeBmp(Fixture("rgb24.bmp")),Expected("rgb24")),"24-bit BMP fixture");
        Check(Same(Image::DecodeTga(Fixture("rgba32.tga")),Expected("rgba32")),"32-bit TGA fixture");
        Check(Same(Image::DecodeTga(Fixture("rgb24rle.tga")),Expected("rgb24")),"RLE TGA fixture");
        for(const auto& image:{Pattern(17,9,false),Pattern(17,9,true),Image::Rgba(300,2,{1,2,3,255})})
        {
            Check(Same(Image::DecodeBmp(Image::EncodeBmp(image)),image),"bmp round trip");
            Check(Same(Image::DecodeTga(Image::EncodeTga(image)),image),"tga round trip");
            Check(Same(Image::DecodeTga(Image::EncodeTga(image,true)),image),"tga rle round trip");
            Check(Same(Image::Decode(Image::EncodeTga(image,true)),image),"decode sniffs tga");
        }
        Check(Image::EncodeTga(Image::Rgba(300,2,{1,2,3,255}),true).size()<60,"rle shrinks runs");
        Check(Image::Format(Fixture("rgb24.bmp"))=="bmp" && Image::Format(Fixture("rgba8.png"))=="png" && Image::Format(Fixture("rgba32.tga"))=="tga","format sniffing");
        Reject([&]{Image::Decode(std::string("\xFF\xD8\xFF\xE0 JFIF",10));},"jpeg");
        Reject([&]{Image::Decode("plain text that is not an image");},"not a PNG");
        Reject([&]{Image::DecodeBmp(Fixture("rgb24.bmp").substr(0,100));},"truncated");

        // Resizing.
        {
            Image::Rgba checker(4,4);
            for(int y=0;y<4;++y)for(int x=0;x<4;++x){auto* p=checker.At(x,y);p[0]=p[1]=p[2]=((x+y)&1)?255:0;}
            const auto half=Image::Resize(checker,2,2,Image::Filter::Box);
            Check(half.width==2 && std::abs(half.At(1,1)[0]-128)<=1,"box filter averages");
            Image::Rgba solid(3,3,{40,80,120,255});
            const auto up=Image::Resize(solid,10,7,Image::Filter::Bilinear);
            Check(up.width==10 && up.height==7 && up.At(9,6)[0]==40 && up.At(4,3)[2]==120,"bilinear keeps a flat colour");
            const auto nearest=Image::Resize(checker,8,8,Image::Filter::Nearest);
            Check(nearest.At(0,0)[0]==0 && nearest.At(2,0)[0]==255,"nearest upscales");
            // Transparent pixels do not darken what is left.
            Image::Rgba edge(2,1,{255,255,255,255});edge.At(1,0)[0]=0;edge.At(1,0)[3]=0;
            const auto one=Image::Resize(edge,1,1);
            Check(one.At(0,0)[0]==255 && std::abs(one.At(0,0)[3]-128)<=1,"alpha-weighted average");
            const auto fit=Image::FitWithin(Pattern(400,100,false),128);
            Check(fit.width==128 && fit.height==32,"fit within");
            Check(Image::FitWithin(solid,128).width==3,"fit never upscales");
            const auto crop=Image::Crop(Pattern(20,20,false),5,6,4,3);
            Check(crop.width==4 && crop.At(0,0)[0]==Pattern(20,20,false).At(5,6)[0],"crop");
            Reject([&]{Image::Crop(solid,2,2,2,2);},"outside");
            Check(Image::FilterNamed("bilinear")==Image::Filter::Bilinear,"filter names");
            Reject([&]{Image::FilterNamed("lanczos");});
            Check(Image::NearestPowerOfTwo(300)==256 && Image::NearestPowerOfTwo(400)==512 && Image::NearestPowerOfTwo(1)==1,"power of two");
        }
        Reject([&]{Image::Rgba(70000,2);},"outside");
        std::cout<<"ImageCodecTests passed\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"ImageCodecTests failed: "<<e.what()<<"\n";return 1;}
}
