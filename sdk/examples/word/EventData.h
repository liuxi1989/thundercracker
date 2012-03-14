#ifndef EVENTDATA_H
#define EVENTDATA_H

#include <sifteo.h>
#include "CubeStateMachine.h"
using namespace Sifteo;

union EventData
{
    EventData() {}
    struct
    {
        char mWord[MAX_LETTERS_PER_WORD + 1];
        unsigned char mPuzzlePieceIndexes[NUM_CUBES];
        int mOffLengthIndex;
        unsigned mNumAnagrams;
        unsigned mNumBonusAnagrams;
        bool mLeadingSpaces;
    } mNewAnagram;

    struct
    {
        const char* mWord;
        Cube::ID mCubeIDStart;
        bool mBonus;
    } mWordFound; // used for NewWordFound and OldWordFound

    struct
    {
        Cube::ID mCubeIDStart;
    } mWordBroken;

    struct
    {
        bool mFirst;
        unsigned mPreviousStateIndex;
    } mEnterState;    

    struct
    {
        unsigned mPreviousStateIndex;
        unsigned mNewStateIndex;
    } mGameStateChanged;

    struct
    {
        Cube::ID mCubeID;
    } mInput; // also tilt

    struct
    {
        char mHintSolution[NUM_CUBES][MAX_LETTERS_PER_CUBE];
    } mHintSolutionUpdate;
};

#endif // EVENTDATA_H
