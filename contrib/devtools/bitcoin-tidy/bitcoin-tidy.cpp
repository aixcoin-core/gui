// Copyright (c) 2023 Aix Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "nontrivial-threadlocal.h"

#include <clang-tidy/ClangTidyModule.h>
#include <clang-tidy/ClangTidyModuleRegistry.h>

class AixModule final : public clang::tidy::ClangTidyModule
{
public:
    void addCheckFactories(clang::tidy::ClangTidyCheckFactories& CheckFactories) override
    {
        CheckFactories.registerCheck<aix::NonTrivialThreadLocal>("aix-nontrivial-threadlocal");
    }
};

static clang::tidy::ClangTidyModuleRegistry::Add<AixModule>
    X("aix-module", "Adds aix checks.");

volatile int AixModuleAnchorSource = 0;
