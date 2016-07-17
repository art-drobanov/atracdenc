#include <iostream>
#include <string>

#include <getopt.h>

#include "pcmengin.h"
#include "wav.h"
#include "aea.h"
#include "atracdenc.h"

using std::cout;
using std::cerr;
using std::endl;
using std::string;
using std::unique_ptr;
using std::move;
using std::stoi;

using namespace NAtracDEnc;


static void printUsage(const char* myName) {
    cout << "\tusage: " << myName << " <-e|-d> <-i input> <-o output>\n" << endl;
    cout << "-e encode mode (PCM -> ATRAC), -i wav file, -o aea file" << endl;
    cout << "-d decode mode (ATRAC -> PCM), -i aea file, -o wav file" << endl;
    cout << "-h get help" << endl;

}

static void printProgress(int percent) {
    static uint32_t counter;
    counter++;
    const char symbols[4] = {'-', '\\', '|', '/'};
    cout << symbols[counter % 4]<< "  "<< percent <<"% done\r";
    fflush(stdout);
}

static string GetHelp() {
    return "\n--encode -i \t - encode mode"
        "\n--decode -d \t - decode mode"
        "\n -i input file"
        "\n -o output file"
        "\n --bitrate (only if supported by codec)"
        "\nAdvanced options:\n --bfuidxconst\t Set constant amount of used BFU. WARNING: It is not a lowpass filter! Do not use it to cut off hi frequency."
        "\n --bfuidxfast\t enable fast search of BFU amount"
        "\n --notransient[=mask] disable transient detection and use optional mask to set bands with short MDCT window";
}

static int checkedStoi(const char* data, int min, int max, int def) {
    int tmp = 0;
    try {
        tmp = stoi(data);
        if (tmp < min || tmp > max)
            throw std::invalid_argument(data);
        return tmp;
    } catch (std::invalid_argument&) {
        cerr << "Wrong arg: " << data << " " << def << " will be used" << endl;
        return def;
    }
}

int main(int argc, char* const* argv) {
    const char* myName = argv[0];
    static struct option longopts[] = {
        { "encode", optional_argument, NULL, 'e' },
        { "decode", no_argument, NULL, 'd' },
        { "help", no_argument, NULL, 'h' },
        { "bitrate", required_argument, NULL, 'b'},
        { "bfuidxconst", required_argument, NULL, 1},
        { "bfuidxfast", no_argument, NULL, 2},
        { "notransient", optional_argument, NULL, 3},
        { "nostdout", no_argument, NULL, 4},
        { NULL, 0, NULL, 0}
    };

    int ch = 0;
    string inFile;
    string outFile;
    uint32_t mode = 0;
    uint32_t bfuIdxConst = 0; //0 - auto, no const
    bool fastBfuNumSearch = false;
    bool nostdout = false;
    TAtrac1EncodeSettings::EWindowMode windowMode = TAtrac1EncodeSettings::EWindowMode::EWM_AUTO;
    uint32_t winMask = 0; //0 - all is long
    uint32_t bitrate = 0; //0 - use default for codec
    while ((ch = getopt_long(argc, argv, "edhi:o:m", longopts, NULL)) != -1) {
        switch (ch) {
            case 'e':
                mode |= E_ENCODE;
                if (optarg) {
                    if (strcmp(optarg, "atrac3") == 0) {
                        mode |= E_ATRAC3;
                    }
                }
                break;
            case 'd':
                mode |= E_DECODE;
                break;
            case 'i':
                inFile = optarg;
                break;
            case 'o':
                outFile = optarg;
                if (outFile == "-")
                    nostdout = true;
                break;
            case 'h':
                cout << GetHelp() << endl;
                return 0;
                break;
            case 'b':
                bitrate = checkedStoi(optarg, 32, 384, 0);
                break;
            case 1:
                bfuIdxConst = checkedStoi(optarg, 1, 8, 0);
                break;
            case 2:
                fastBfuNumSearch = true;
                break;
            case 3:
                windowMode = TAtrac1EncodeSettings::EWindowMode::EWM_NOTRANSIENT;
                if (optarg) {
                    winMask = stoi(optarg);
                }
                cout << "Transient detection disabled, bands: low - " <<
                    ((winMask & 1) ? "short": "long") << ", mid - " <<
                    ((winMask & 2) ? "short": "long") << ", hi - " <<
                    ((winMask & 4) ? "short": "long") << endl;
                break;
            case 4:
                nostdout = true;
                break;
			default:
                printUsage(myName);
        }

    }
    argc -= optind;
    argv += optind;

    if (inFile.empty()) {
        cerr << "No input file" << endl;
        return 1;
    }
    if (outFile.empty()) {
        cerr << "No out file" << endl;
        return 1;
    }
    if (bfuIdxConst > 8) {
        cerr << "Wrong bfuidxconst value ("<< bfuIdxConst << "). This is advanced options, use --help to get more information" << endl;
        return 1;
    }

    TPCMEngine<double>* pcmEngine = nullptr;
    IProcessor<double>* atracProcessor;
    uint64_t totalSamples = 0;
    TWavPtr wavIO;
    uint32_t pcmFrameSz = 0; //size of one pcm frame to process
    if (mode == E_ENCODE) {
        wavIO = TWavPtr(new TWav(inFile));
        const int numChannels = wavIO->GetChannelNum();
        totalSamples = wavIO->GetTotalSamples();
        //TODO: recheck it
        TAeaPtr aeaIO = TAeaPtr(new TAea(outFile, "test", numChannels, numChannels * totalSamples / 512));
        pcmEngine = new TPCMEngine<double>(4096, numChannels, TPCMEngine<double>::TReaderPtr(wavIO->GetPCMReader<double>()));
        if (!nostdout)
            cout << "Input file: " << inFile << "\n Channels: " << numChannels << "\n SampleRate: " << wavIO->GetSampleRate() << "\n TotalSamples: " << totalSamples << endl;
        atracProcessor = new TAtrac1Processor(move(aeaIO), TAtrac1EncodeSettings(bfuIdxConst, fastBfuNumSearch, windowMode, winMask));
        pcmFrameSz = 512;
    } else if (mode == E_DECODE) {
        TAeaPtr aeaIO = TAeaPtr(new TAea(inFile));
        totalSamples = aeaIO->GetLengthInSamples();
        uint32_t length = aeaIO->GetLengthInSamples();
        if (!nostdout)
            cout << "Name: " << aeaIO->GetName() << "\n Channels: " << aeaIO->GetChannelNum() << "\n Length: " << length << endl;
        wavIO = TWavPtr(new TWav(outFile, aeaIO->GetChannelNum(), 44100));
        pcmEngine = new TPCMEngine<double>(4096, aeaIO->GetChannelNum(), TPCMEngine<double>::TWriterPtr(wavIO->GetPCMWriter<double>()));
        atracProcessor = new TAtrac1Processor(move(aeaIO), TAtrac1EncodeSettings(bfuIdxConst, fastBfuNumSearch, windowMode, winMask));
        pcmFrameSz = 512;
    } else if (mode == (E_ENCODE | E_ATRAC3)) {
        std::cout << "WARNING: ATRAC3 is uncompleted mode (no psy, tonal encoding, gc), result will be not good )))" << std::endl;
        const TContainerParams* atrac3params = TAtrac3Data::GetContainerParamsForBitrate(bitrate*1024);
        if (atrac3params == nullptr) {
            std::cerr << "wrong atrac3 params, exiting" << std::endl;
            return 1;
        }
        std::cout << "bitrate " << atrac3params->Bitrate << std::endl;
        wavIO = TWavPtr(new TWav(inFile));
        const int numChannels = wavIO->GetChannelNum();
        totalSamples = wavIO->GetTotalSamples();
        TAeaPtr omaIO = TAeaPtr(new TOma(outFile, "test", numChannels, numChannels * totalSamples / 512, OMAC_ID_ATRAC3, atrac3params->FrameSz));
        pcmEngine = new TPCMEngine<double>(4096, numChannels, TPCMEngine<double>::TReaderPtr(wavIO->GetPCMReader<double>()));
        atracProcessor = new TAtrac3Processor(move(omaIO), *atrac3params);
        pcmFrameSz = 1024;
    } else {
        cerr << "Processing mode was not specified" << endl;
        return 1;
    }

    auto atracLambda = (mode == E_DECODE) ? atracProcessor->GetDecodeLambda() :
        atracProcessor->GetEncodeLambda();

    uint64_t processed = 0;
    try {
        while (totalSamples > (processed = pcmEngine->ApplyProcess(pcmFrameSz, atracLambda)))
        {
            if (!nostdout)
                printProgress(processed*100/totalSamples);
        }
        if (!nostdout)
            cout << "\nDone" << endl;
    }
    catch (TAeaIOError err) {
        cerr << "Aea IO fatal error: " << err.what() << endl;
        exit(1);
    }
    catch (TWavIOError err) {
        cerr << "Wav IO fatal error: " << err.what() << endl;
        exit(1);
    }
}
