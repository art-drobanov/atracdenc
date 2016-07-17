#include "atrac_scale.h"
#include "atrac1.h"
#include "atrac3.h"
#include <cmath>
#include <iostream>
#include <algorithm>

namespace NAtracDEnc {

using std::vector;
using std::map;

using std::cerr;
using std::endl;

using std::abs;

static const uint32_t MAX_SCALE = 65536;

template<class TBaseData>
TScaledBlock TScaler<TBaseData>::Scale(const TFloat* in, uint16_t len) {
    TFloat maxAbsSpec = 0;
    for (uint16_t i = 0; i < len; ++i) {
        const TFloat absSpec = abs(in[i]);
        if (absSpec > maxAbsSpec) {
            if (absSpec > MAX_SCALE) {
                cerr << "Scale error: absSpec > MAX_SCALE, val: " << absSpec << endl;
                maxAbsSpec = MAX_SCALE;
            } else {
                maxAbsSpec = absSpec;
            }
        }
    }
    const map<TFloat, uint8_t>::const_iterator scaleIter = ScaleIndex.lower_bound(maxAbsSpec);
    const TFloat scaleFactor = scaleIter->first;
    const uint8_t scaleFactorIndex = scaleIter->second;
    TScaledBlock res(scaleFactorIndex);
    for (uint16_t i = 0; i < len; ++i) {
        const TFloat scaledValue = in[i] / scaleFactor;
        if (scaledValue > 1.0) {
            cerr << "got "<< scaledValue << " it is wrong scalling" << endl;
        }
        res.Values.push_back(scaledValue);
	}
    return res;
}

template<class TBaseData>
vector<TScaledBlock> TScaler<TBaseData>::ScaleFrame(const vector<TFloat>& specs, const TBlockSize& blockSize) {
    vector<TScaledBlock> scaledBlocks;
    for (uint8_t bandNum = 0; bandNum < this->NumQMF; ++bandNum) {
        const bool shortWinMode = !!blockSize.LogCount[bandNum];
        for (uint8_t blockNum = this->BlocksPerBand[bandNum]; blockNum < this->BlocksPerBand[bandNum + 1]; ++blockNum) {
            const uint16_t specNumStart = shortWinMode ? TBaseData::SpecsStartShort[blockNum] : 
                                                         TBaseData::SpecsStartLong[blockNum];
            scaledBlocks.emplace_back(Scale(&specs[specNumStart], this->SpecsPerBlock[blockNum]));
		}
	}
    return scaledBlocks;
}

template
class TScaler<NAtrac1::TAtrac1Data>;

template
class TScaler<NAtrac3::TAtrac3Data>;

} //namespace NAtracDEnc
