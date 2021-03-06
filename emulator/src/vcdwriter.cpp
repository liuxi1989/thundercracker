/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * Sifteo Thundercracker simulator
 * Micah Elizabeth Scott <micah@misc.name>
 *
 * Copyright <c> 2012 Sifteo, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "vcdwriter.h"
#include "vtime.h"


void VCDWriter::enterScope(const std::string scope)
{
    defs << "$scope module " << scope << " $end\n";
}

void VCDWriter::leaveScope()
{
    defs << "$upscope $end\n";
}

void VCDWriter::setNamePrefix(const std::string prefix)
{
    namePrefix = prefix;
}

void VCDWriter::define(const std::string name, void *var, unsigned numBits, unsigned firstBit)
{
    std::string identifier = createIdentifier(sources.size());
    identifiers.push_back(identifier);
    
    SignalSource s(var, numBits, firstBit);
    sources.push_back(s);

    defs << "$var reg " << numBits << " " << identifier << " " << namePrefix << name;
    if (numBits > 1)
        defs << "[" << (numBits - 1) << ":0]";
    defs << " $end\n";
}

void VCDWriter::writeHeader(FILE *f)
{
    fprintf(f, "$timescale\n  %"PRIu64" fs\n$end\n%s$enddefinitions $end\n",
        ((uint64_t)1e15) / VirtualTime::HZ, defs.str().c_str());
}

void VCDWriter::writeTick(FILE *f, uint64_t clock)
{
    for (unsigned id = 0; id < sources.size(); id++) {
        SignalSource &source = sources[id];
        uint64_t newValue = source.sample();
        
        if (newValue != source.value) {
            if (clock != currentTick) {
                fprintf(f, "#%"PRIu64"\n", clock);
                currentTick = clock;
            }
            
            if (source.numBits > 1)
                fprintf(f, "b");
            for (int bit = source.numBits - 1; bit >= 0; bit--)
                fprintf(f, "%u", (unsigned)((newValue >> bit) & 1));
            fprintf(f, " %s\n", identifiers[id].c_str());            

            source.value = newValue;
        }
    }
}

std::string VCDWriter::createIdentifier(unsigned id)
{
    /*
     * Create a signal ID string based on the numerical ID 'i'.
     */

    const char firstSymbol = '!';
    const char lastSymbol = '}';
    const unsigned base = lastSymbol - firstSymbol + 1;
    std::string s;

    do {
        s += (char)(firstSymbol + (id % base));
        id /= base;
    } while (id);

    return s;
}
