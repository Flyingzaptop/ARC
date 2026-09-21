#include <algorithm>
#include <cmath>
#include <cstddef>
// The Python worker supplies already filtered float64 moments and linear RGB.
// Fuse pointwise SSIM/error/tile reductions without full-frame temporaries.
extern "C" __declspec(dllexport) int ArcQualityStripe(
    const double* ma,const double* mb,const double* aa,const double* bb,const double* ab,
    const double* reference,const double* candidate,unsigned width,unsigned rows,unsigned halo,
    unsigned valid_begin,unsigned valid_end,double* tiles,double* totals) noexcept {
    if(!ma||!mb||!aa||!bb||!ab||!reference||!candidate||!tiles||!totals||width<11||width>7680||!rows||rows>512||halo>5||valid_begin>valid_end||valid_end>rows)return 1;
    const auto columns=(width+7)/8,tile_rows=(rows+7)/8;
    std::fill(tiles,tiles+std::size_t(columns)*tile_rows,0.);std::fill(totals,totals+3,0.);
    for(unsigned y=0;y<rows;++y)for(unsigned x=0;x<width;++x){
        const auto pixel=(std::size_t(y)*width+x)*3;double error=0;
        for(unsigned channel=0;channel<3;++channel){const double d=std::abs(reference[pixel+channel]-candidate[pixel+channel]);error+=d;totals[2]=std::max(totals[2],d);}
        totals[1]+=error;tiles[std::size_t(y/8)*columns+x/8]+=error;
        if(y<valid_begin||y>=valid_end||x<5||x>=width-5)continue;
        const auto index=std::size_t(y+halo)*width+x;const double a=ma[index],b=mb[index];
        const auto va=std::max(0.,aa[index]-a*a),vb=std::max(0.,bb[index]-b*b),cov=ab[index]-a*b;
        totals[0]+=((2*a*b+.0001)*(2*cov+.0009))/((a*a+b*b+.0001)*(va+vb+.0009));
    }
    return 0;
}
