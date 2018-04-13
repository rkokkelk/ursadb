#pragma once

#include <vector>
#include <string>
#include <fstream>

#include "Core.h"
#include "Utils.h"


class IndexBuilder {
    std::vector<std::vector<FileId>> raw_index;

public:
    IndexBuilder();

    void add_trigram(FileId fid, TriGram val);
    void save(const std::string &fname);
};