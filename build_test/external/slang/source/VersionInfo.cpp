//------------------------------------------------------------------------------
// VersionInfo.cpp
// Input file for build-time version source generation
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "slang/util/VersionInfo.h"

using std::string_view;
using namespace std::literals;

namespace slang {

int VersionInfo::getMajor() {
    return 9;
}

int VersionInfo::getMinor() {
    return 1;
}

int VersionInfo::getPatch() {
    return 111;
}

string_view VersionInfo::getHash() {
    return "301723fe5"sv;
}

}
