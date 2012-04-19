////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2012 - TRACER. All rights reserved.
////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef SIFTEO_BUDDIES_BOOK_H_
#define SIFTEO_BUDDIES_BOOK_H_

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "BuddyId.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Buddies {

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

struct Book
{
    Book(const char *title = 0, unsigned int numPuzzles = 0, BuddyId unlockBuddyId = BUDDY_GLUV)
        : mTitle(title)
        , mNumPuzzles(numPuzzles)
        , mUnlockBuddyId(unlockBuddyId)
    {
    }
    
    const char *mTitle;
    unsigned int mNumPuzzles;
    BuddyId mUnlockBuddyId;
};

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

}

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////

#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////