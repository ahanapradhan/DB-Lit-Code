#include <algorithm>
#include <climits>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

using PageId = long long;

constexpr int K = 2;

long long CORRELATED_REF_PERIOD = 0;
long long RETAINED_INFO_PERIOD = LLONG_MAX / 4;
size_t BUFFER_SIZE = 60;
bool VERBOSE = false;

struct History {
    // HIST[1] is the latest uncorrelated reference.
    // HIST[2] is the second-latest uncorrelated reference.
    long long HIST[K + 1] = {0, 0, 0};

    // Most recent physical reference, correlated or not.
    long long LAST = 0;

    bool inBuffer = false;
};

map<PageId, History> histories;
vector<PageId> bufferPool;

bool isResident(PageId page)
{
    return find(bufferPool.begin(), bufferPool.end(), page)
           != bufferPool.end();
}

void purgeOldHistory(long long currentTime)
{
    vector<PageId> expiredPages;

    for (const auto &[page, history] : histories) {
        if (!history.inBuffer &&
            currentTime - history.LAST > RETAINED_INFO_PERIOD) {
            expiredPages.push_back(page);
        }
    }

    for (PageId page : expiredPages) {
        if (VERBOSE) {
            cout << "      [purge] HIST(" << page
                 << ") discarded\n";
        }

        histories.erase(page);
    }
}

void printBuffer()
{
    if (!VERBOSE) {
        return;
    }

    cout << "      Buffer = { ";

    for (PageId page : bufferPool) {
        cout << page << ' ';
    }

    cout << "}\n";
}

/*
 * Invoke this function for a reference to page p at logical time t.
 *
 * Return value:
 *   true  = buffer hit
 *   false = buffer miss
 */
bool referencePage(PageId page, long long currentTime)
{
    const auto position =
        find(bufferPool.begin(), bufferPool.end(), page);

    const bool hit = position != bufferPool.end();

    if (VERBOSE) {
        cout << "t=" << setw(6) << currentTime
             << " ref=" << setw(8) << page;
    }

    if (hit) {
        History &history = histories.at(page);

        /*
         * Determine correlation using LAST, because LAST records every
         * physical access, including correlated accesses.
         */
        if (currentTime - history.LAST >
            CORRELATED_REF_PERIOD) {

            /*
             * The old correlated-reference period ran from
             * HIST[1] through LAST.
             */
            const long long correlatedPeriodLength =
                history.LAST - history.HIST[1];

            /*
             * Shift the uncorrelated history, adjusting earlier
             * timestamps to discount the collapsed correlated period.
             *
             * For K = 2:
             * HIST[2] = old HIST[1] + correlatedPeriodLength
             *         = old LAST
             */
            for (int i = K; i >= 2; --i) {
                if (history.HIST[i - 1] == 0) {
                    history.HIST[i] = 0;
                } else {
                    history.HIST[i] =
                        history.HIST[i - 1] +
                        correlatedPeriodLength;
                }
            }

            history.HIST[1] = currentTime;

            if (VERBOSE) {
                cout << " [HIT, uncorrelated]\n";
            }
        } else {
            /*
             * A correlated reference updates LAST, but does not
             * create a new LRU-K history entry.
             */
            if (VERBOSE) {
                cout << " [HIT, correlated]\n";
            }
        }

        history.LAST = currentTime;
    } else {
        if (VERBOSE) {
            cout << " [MISS]\n";
        }

        /*
         * If the buffer is full, choose a replacement victim.
         */
        if (bufferPool.size() >= BUFFER_SIZE) {
            PageId victim = -1;
            bool foundEligibleVictim = false;

            /*
             * Maximizing backward K-distance is equivalent to choosing
             * the smallest HIST[K] timestamp.
             *
             * HIST[K] == 0 means the page has fewer than K qualifying
             * references, so its backward K-distance is infinity.
             * Therefore, zero is the smallest and strongest eviction key.
             */
            long long smallestHistoryTimestamp = LLONG_MAX;

            for (PageId candidate : bufferPool) {
                const History &candidateHistory =
                    histories.at(candidate);

                const bool eligible =
                    currentTime - candidateHistory.LAST >
                    CORRELATED_REF_PERIOD;

                if (!eligible) {
                    continue;
                }

                const long long historyKey =
                    candidateHistory.HIST[K];

                if (!foundEligibleVictim ||
                    historyKey < smallestHistoryTimestamp) {
                    victim = candidate;
                    smallestHistoryTimestamp = historyKey;
                    foundEligibleVictim = true;
                } else if (
                    historyKey == smallestHistoryTimestamp &&
                    candidateHistory.LAST <
                    histories.at(victim).LAST) {
                    /*
                     * Subsidiary LRU policy for equal LRU-K keys,
                     * particularly when multiple pages have
                     * HIST[K] == 0.
                     */
                    victim = candidate;
                }
            }

            /*
             * With a positive CRP and a very small buffer, it is
             * possible that every page is still protected by its
             * correlated-reference period.
             *
             * For experiments, use ordinary LRU as a deterministic
             * emergency fallback so that the workload can proceed.
             */
            if (!foundEligibleVictim) {
                victim = bufferPool.front();

                for (PageId candidate : bufferPool) {
                    if (histories.at(candidate).LAST <
                        histories.at(victim).LAST) {
                        victim = candidate;
                    }
                }

                if (VERBOSE) {
                    cout << "      No page outside CRP; "
                         << "emergency LRU victim = "
                         << victim << "\n";
                }
            } else if (VERBOSE) {
                cout << "      LRU-" << K
                     << " victim = " << victim
                     << ", HIST(" << K << ") = "
                     << smallestHistoryTimestamp << "\n";
            }

            auto victimPosition =
                find(bufferPool.begin(),
                     bufferPool.end(),
                     victim);

            if (victimPosition == bufferPool.end()) {
                throw logic_error(
                    "Selected victim is not resident");
            }

            bufferPool.erase(victimPosition);
            histories.at(victim).inBuffer = false;
        } else if (VERBOSE) {
            cout << "      Free buffer frame used\n";
        }

        /*
         * Fetch the requested page.
         */
        bufferPool.push_back(page);

        auto historyPosition = histories.find(page);

        if (historyPosition == histories.end()) {
            /*
             * No retained history exists.
             */
            History newHistory;
            newHistory.HIST[1] = currentTime;
            newHistory.HIST[2] = 0;
            newHistory.LAST = currentTime;
            newHistory.inBuffer = true;

            histories.emplace(page, newHistory);
        } else {
            /*
             * The page was nonresident, but retained history exists.
             *
             * The new reference starts a new residence and is counted
             * as a new history observation.
             */
            History &history = historyPosition->second;

            for (int i = K; i >= 2; --i) {
                history.HIST[i] = history.HIST[i - 1];
            }

            history.HIST[1] = currentTime;
            history.LAST = currentTime;
            history.inBuffer = true;
        }
    }

    purgeOldHistory(currentTime);
    printBuffer();

    return hit;
}

long long readLongLong(
    int argc,
    char *argv[],
    int position,
    long long defaultValue)
{
    if (position >= argc) {
        return defaultValue;
    }

    return stoll(argv[position]);
}

unsigned long long readUnsignedLongLong(
    int argc,
    char *argv[],
    int position,
    unsigned long long defaultValue)
{
    if (position >= argc) {
        return defaultValue;
    }

    return stoull(argv[position]);
}

void resetExperiment()
{
    histories.clear();
    bufferPool.clear();
}

int main(int argc, char *argv[])
{
    try {
    for (int loop = 60; loop <= 460; loop+=20) {

            const long long BInput =
                readLongLong(argc, argv, 1, loop);

            const long long pool1Size =
                readLongLong(argc, argv, 2, 100);

            const long long pool2Size =
                readLongLong(argc, argv, 3, 10000);

            CORRELATED_REF_PERIOD =
                readLongLong(argc, argv, 4, 4);

            const long long retainedInput =
                readLongLong(argc, argv, 5, 200);

            const unsigned long long seed =
                readUnsignedLongLong(argc, argv, 6, 72);

            VERBOSE =
                readLongLong(argc, argv, 7, 0) != 0;

            if (BInput < 1) {
                throw invalid_argument(
                    "Buffer size B must be at least 1");
            }

            if (pool1Size < 1 ||
                pool2Size < 1 ||
                pool1Size >= pool2Size) {
                throw invalid_argument(
                    "Require 1 <= N1 < N2");
                }

            if (CORRELATED_REF_PERIOD < 0) {
                throw invalid_argument(
                    "CRP cannot be negative");
            }

            BUFFER_SIZE = static_cast<size_t>(BInput);

            const long long warmupReferences =
                10 * pool1Size;

            const long long measuredReferences =
                30 * pool1Size;

            const long long totalReferences =
                warmupReferences + measuredReferences;

            /*
             * RIP = -1 retains all history for the complete run.
             *
             * This is the cleanest default for comparing replacement
             * behavior because it prevents history expiration from
             * becoming an additional experimental variable.
             */
            if (retainedInput == -1) {
                RETAINED_INFO_PERIOD = totalReferences + 1;
            } else if (retainedInput < 0) {
                throw invalid_argument(
                    "RIP must be nonnegative, or -1");
            } else {
                RETAINED_INFO_PERIOD = retainedInput;
            }

            resetExperiment();

            mt19937_64 generator(seed);

            /*
             * Pool 1 page IDs:
             *   1 through N1
             *
             * Pool 2 page IDs:
             *   N1 + 1 through N1 + N2
             */
            uniform_int_distribution<PageId> selectPool1(
                1,
                pool1Size);

            uniform_int_distribution<PageId> selectPool2(
                pool1Size + 1,
                pool1Size + pool2Size);

            long long measuredHits = 0;
            long long measuredMisses = 0;

            long long pool1MeasuredHits = 0;
            long long pool1MeasuredMisses = 0;

            long long pool2MeasuredHits = 0;
            long long pool2MeasuredMisses = 0;

            for (long long index = 0;
                 index < totalReferences;
                 ++index) {

                const long long currentTime = index + 1;

                /*
                 * Strict alternation:
                 *
                 * even index: Pool 1
                 * odd index:  Pool 2
                 */
                const bool pool1Turn =
                    index % 2 == 0;

                const PageId page =
                    pool1Turn
                        ? selectPool1(generator)
                        : selectPool2(generator);

                const bool hit =
                    referencePage(page, currentTime);

                /*
                 * Discard warm-up observations.
                 */
                if (index < warmupReferences) {
                    continue;
                }

                if (hit) {
                    ++measuredHits;

                    if (pool1Turn) {
                        ++pool1MeasuredHits;
                    } else {
                        ++pool2MeasuredHits;
                    }
                } else {
                    ++measuredMisses;

                    if (pool1Turn) {
                        ++pool1MeasuredMisses;
                    } else {
                        ++pool2MeasuredMisses;
                    }
                }
                 }

            const long long pool1MeasuredReferences =
                pool1MeasuredHits + pool1MeasuredMisses;

            const long long pool2MeasuredReferences =
                pool2MeasuredHits + pool2MeasuredMisses;

            const double overallHitRatio =
                static_cast<double>(measuredHits) /
                static_cast<double>(measuredReferences);

            const double pool1HitRatio =
                pool1MeasuredReferences == 0
                    ? 0.0
                    : static_cast<double>(
                          pool1MeasuredHits) /
                      static_cast<double>(
                          pool1MeasuredReferences);

            const double pool2HitRatio =
                pool2MeasuredReferences == 0
                    ? 0.0
                    : static_cast<double>(
                          pool2MeasuredHits) /
                      static_cast<double>(
                          pool2MeasuredReferences);

            size_t residentPool1Pages = 0;
            size_t residentPool2Pages = 0;

            for (PageId page : bufferPool) {
                if (page <= pool1Size) {
                    ++residentPool1Pages;
                } else {
                    ++residentPool2Pages;
                }
            }

            cout << fixed << setprecision(4);

           /* cout << "Results\n";
            cout << "-----------------------------------\n";
            cout << "Measured hits             = "
                 << measuredHits << '\n';
            cout << "Measured misses           = "
                 << measuredMisses << '\n';*/
            cout << BUFFER_SIZE         << " "
                 << overallHitRatio << '\n';

           /* cout << "Pool 1 measured hits      = "
                 << pool1MeasuredHits << '\n';
            cout << "Pool 1 measured misses    = "
                 << pool1MeasuredMisses << '\n';
            cout << "Pool 1 hit ratio          = "
                 << pool1HitRatio << '\n';

            cout << "Pool 2 measured hits      = "
                 << pool2MeasuredHits << '\n';
            cout << "Pool 2 measured misses    = "
                 << pool2MeasuredMisses << '\n';
            cout << "Pool 2 hit ratio          = "
                 << pool2HitRatio << '\n';

            cout << "Final Pool 1 residents    = "
                 << residentPool1Pages << '\n';
            cout << "Final Pool 2 residents    = "
                 << residentPool2Pages << '\n';
            cout << "Final total residents     = "
                 << bufferPool.size() << '\n';*/

            //return 0;

    }
    } catch (const exception &error) {
        cerr << "Error: " << error.what() << "\n\n";

        cerr << "Usage:\n"
             << "  " << argv[0]
             << " [B=60]"
             << " [N1=100]"
             << " [N2=10000]"
             << " [CRP=0]"
             << " [RIP=-1]"
             << " [seed=42]"
             << " [verbose=0]\n";

        return 1;
    }
}
