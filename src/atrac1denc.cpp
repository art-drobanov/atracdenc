#include <vector>

#include "atrac1denc.h"
#include "bitstream/bitstream.h"
#include "atrac/atrac1.h"
#include "atrac/atrac1_dequantiser.h"
#include "atrac/atrac1_qmf.h"
#include "atrac/atrac1_bitalloc.h"
#include "util.h"

namespace NAtracDEnc {
using namespace NBitStream;
using namespace NAtrac1;
using namespace NMDCT;
using std::vector;

TAtrac1Processor::TAtrac1Processor(TCompressedIOPtr&& aea, TAtrac1EncodeSettings&& settings)
    : Aea(std::move(aea))
    , Settings(std::move(settings))
{
}

static void vector_fmul_window(TFloat *dst, const TFloat *src0,
                                const TFloat *src1, const TFloat *win, int len)
{
    int i, j;

    dst  += len;
    win  += len;
    src0 += len;

    for (i = -len, j = len - 1; i < 0; i++, j--) {
        TFloat s0 = src0[i];
        TFloat s1 = src1[j];
        TFloat wi = win[i];
        TFloat wj = win[j];
        dst[i] = s0 * wj - s1 * wi;
        dst[j] = s0 * wi + s1 * wj;
    }
}

vector<TFloat> midct(TFloat* x, int N) {
    vector<TFloat> res;
    for (int n = 0; n < 2 * N; n++) {
        TFloat sum = 0;
        for (int k = 0; k < N; k++) {
            sum += (x[k] * cos((M_PI/N) * ((TFloat)n + 0.5 + N/2) * ((TFloat)k + 0.5)));
        }

        res.push_back(sum);
    }
    return res;
}

void TAtrac1MDCT::Mdct(TFloat Specs[512], TFloat* low, TFloat* mid, TFloat* hi, const TBlockSize& blockSize) {
    uint32_t pos = 0;
    for (uint32_t band = 0; band < NumQMF; band++) {
        const uint32_t numMdctBlocks = 1 << blockSize.LogCount[band];
        TFloat* srcBuf = (band == 0) ? low : (band == 1) ? mid : hi;
        uint32_t bufSz = (band == 2) ? 256 : 128; 
        const uint32_t blockSz = (numMdctBlocks == 1) ? bufSz : 32;
        uint32_t winStart = (numMdctBlocks == 1) ? ((band == 2) ? 112 : 48) : 0;
        //compensate level for 3rd band in case of short window
        const TFloat multiple = (numMdctBlocks != 1 && band == 2) ? 2.0 : 1.0;
        vector<TFloat> tmp(512);
        uint32_t blockPos = 0;

        for (size_t k = 0; k < numMdctBlocks; ++k) {
            memcpy(&tmp[winStart], &srcBuf[bufSz], 32 * sizeof(TFloat));
            for (size_t i = 0; i < 32; i++) {
                srcBuf[bufSz + i] = TAtrac1Data::SineWindow[i] * srcBuf[blockPos + blockSz - 32 + i];
                srcBuf[blockPos + blockSz - 32 + i] = TAtrac1Data::SineWindow[31 - i] * srcBuf[blockPos + blockSz - 32 + i];
            }
            memcpy(&tmp[winStart+32], &srcBuf[blockPos], blockSz * sizeof(TFloat));
            const vector<TFloat>&  sp = (numMdctBlocks == 1) ? ((band == 2) ? Mdct512(&tmp[0]) : Mdct256(&tmp[0])) : Mdct64(&tmp[0]);
            for (size_t i = 0; i < sp.size(); i++) {
                Specs[blockPos + pos + i] = sp[i] * multiple;
            }
            if (band) {
                SwapArray(&Specs[blockPos + pos], sp.size());
            }

            blockPos += 32;
        }
        pos += bufSz;
    } 
}
void TAtrac1MDCT::IMdct(TFloat Specs[512], const TBlockSize& mode, TFloat* low, TFloat* mid, TFloat* hi) {
    uint32_t pos = 0;
    for (size_t band = 0; band < NumQMF; band++) {
        const uint32_t numMdctBlocks = 1 << mode.LogCount[band];
        const uint32_t bufSz = (band == 2) ? 256 : 128;
        const uint32_t blockSz = (numMdctBlocks == 1) ? bufSz : 32;
        uint32_t start = 0;

        TFloat* dstBuf = (band == 0) ? low : (band == 1) ? mid : hi;

        vector<TFloat> invBuf(512);
        TFloat* prevBuf = &dstBuf[bufSz * 2  - 16];
        for (uint32_t block = 0; block < numMdctBlocks; block++) {
            if (band) {
                SwapArray(&Specs[pos], blockSz);
            }
            vector<TFloat> inv = (numMdctBlocks != 1) ? midct(&Specs[pos], blockSz) : (bufSz == 128) ? Midct256(&Specs[pos]) : Midct512(&Specs[pos]);
            for (size_t i = 0; i < (inv.size()/2); i++) {
                invBuf[start+i] = inv[i + inv.size()/4];
            }

            vector_fmul_window(dstBuf + start, prevBuf, &invBuf[start], &TAtrac1Data::SineWindow[0], 16);

            prevBuf = &invBuf[start+16];
            start += blockSz;
            pos += blockSz;
        }
        if (numMdctBlocks == 1)
            memcpy(dstBuf + 32, &invBuf[16], ((band == 2) ? 240 : 112) * sizeof(TFloat));

        for (size_t j = 0; j < 16; j++) {
            dstBuf[bufSz*2 - 16  + j] = invBuf[bufSz - 16 + j];
        }
    }
}

TPCMEngine<TFloat>::TProcessLambda TAtrac1Processor::GetDecodeLambda() {
    return [this](TFloat* data, const TPCMEngine<TFloat>::ProcessMeta& meta) {
        TFloat sum[512];
        const uint32_t srcChannels = Aea->GetChannelNum();
        for (uint32_t channel = 0; channel < srcChannels; channel++) {
            std::unique_ptr<TAea::TFrame> frame(Aea->ReadFrame());

            TBitStream bitstream(frame->Get(), frame->Size());

            TBlockSize mode(&bitstream);
            TAtrac1Dequantiser dequantiser;
            vector<TFloat> specs;
            specs.resize(512);;
            dequantiser.Dequant(&bitstream, mode, &specs[0]);

            IMdct(&specs[0], mode, &PcmBufLow[channel][0], &PcmBufMid[channel][0], &PcmBufHi[channel][0]);
            SynthesisFilterBank[channel].Synthesis(&sum[0], &PcmBufLow[channel][0], &PcmBufMid[channel][0], &PcmBufHi[channel][0]);
            for (size_t i = 0; i < NumSamples; ++i) {
                if (sum[i] > PcmValueMax)
                    sum[i] = PcmValueMax;
                if (sum[i] < PcmValueMin)
                    sum[i] = PcmValueMin;

                data[i * srcChannels + channel] = sum[i];
            }
        }

    };
}


TPCMEngine<TFloat>::TProcessLambda TAtrac1Processor::GetEncodeLambda() {
    const uint32_t srcChannels = Aea->GetChannelNum();
    vector<IAtrac1BitAlloc*> bitAlloc;
    for (size_t i = 0; i < srcChannels; i++) {
        TAea* atrac1container = dynamic_cast<TAea*>(Aea.get());
        if (atrac1container == nullptr)
            abort();
        bitAlloc.push_back(new TAtrac1SimpleBitAlloc(atrac1container, Settings.GetBfuIdxConst(), Settings.GetFastBfuNumSearch()));
    }

    return [this, srcChannels, bitAlloc](TFloat* data, const TPCMEngine<TFloat>::ProcessMeta& meta) {
        for (uint32_t channel = 0; channel < srcChannels; channel++) {
            TFloat src[NumSamples];
            vector<TFloat> specs(512);
            for (size_t i = 0; i < NumSamples; ++i) {
                src[i] = data[i * srcChannels + channel];
            }

            SplitFilterBank[channel].Split(&src[0], &PcmBufLow[channel][0], &PcmBufMid[channel][0], &PcmBufHi[channel][0]);

            uint32_t windowMask = 0;
            if (Settings.GetWindowMode() == TAtrac1EncodeSettings::EWindowMode::EWM_AUTO) {
                windowMask |= (uint32_t)TransientDetectors.GetDetector(channel, 0).Detect(&PcmBufLow[channel][0]);

                const vector<TFloat>& invMid = InvertSpectr<128>(&PcmBufMid[channel][0]);
                windowMask |= (uint32_t)TransientDetectors.GetDetector(channel, 1).Detect(&invMid[0]) << 1;

                const vector<TFloat>& invHi = InvertSpectr<256>(&PcmBufHi[channel][0]);
                windowMask |= (uint32_t)TransientDetectors.GetDetector(channel, 2).Detect(&invHi[0]) << 2;

                //std::cout << "trans: " << windowMask << std::endl;
            } else {
                //no transient detection, use given mask
                windowMask = Settings.GetWindowMask();
            }
            const TBlockSize blockSize(windowMask & 0x1, windowMask & 0x2, windowMask & 0x4); //low, mid, hi

            Mdct(&specs[0], &PcmBufLow[channel][0], &PcmBufMid[channel][0], &PcmBufHi[channel][0], blockSize);
            bitAlloc[channel]->Write(Scaler.ScaleFrame(specs, blockSize), blockSize);
        }
    };
}

} //namespace NAtracDEnc
