#include "dataset_builder.h"

FileId DatasetBuilder::register_fname(const std::string &fname) {
    auto new_id = (FileId) fids.size();
    fids.push_back(fname);
    return new_id;
}

void DatasetBuilder::add_trigram(const FileId &fid, const TriGram &val) {
    index[val].push_back(fid);
}

std::vector<uint8_t> DatasetBuilder::compress_run(const std::vector <FileId> &files) {
    uint32_t prev = 0;
    std::vector <uint8_t> result;

    for (uint32_t fid : files) {
        uint32_t diff = (fid + 1U) - prev;
        while (diff >= 0x80U) {
            result.push_back((uint8_t)(0x80U | (diff & 0x7FU)));
            diff >>= 7;
        }
        result.push_back((uint8_t) diff);
        prev = fid + 1U;
    }

    return result;
}

void DatasetBuilder::save(const std::string &fname) {
    std::vector <uint32_t> offsets;
    std::ofstream out(fname, std::ofstream::binary);

    char header[4] = {(char) 0xCA, (char) 0x7D, (char) 0xA7, (char) 0xA};
    out.write(header, 4);

    for (int i = 0; i < fids.size(); i++) {
        auto fnsize = (uint16_t) fids[i].size();
        out.write((char *) &fnsize, 2);
        out.write(fids[i].c_str(), fids[i].size());
    }

    char blank[2] = {0, 0};
    out.write(blank, 2);

    for (int i = 0; i < 16777216; i++) {
        offsets.push_back((uint32_t) out.tellp());
        for (auto b : compress_run(index[i])) {
            out.write((char *) &b, 1);
        }
    }

    for (auto offset : offsets) {
        out.write((char *) &offset, 4);
    }

    out.close();
}
