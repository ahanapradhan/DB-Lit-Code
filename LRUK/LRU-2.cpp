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

int main()

{

    try {



        BUFFER_SIZE = 4;

        CORRELATED_REF_PERIOD = 2;

        RETAINED_INFO_PERIOD = 20;

        VERBOSE = true;

        string pageReferenceString = "1 2 3 4 1 2 5 1 2 3 4 5 6 8 7 1 2 3 4 4 1 2 3 6 5 8 7 2 3 2 3 2 3 2 3";

        resetExperiment();

        istringstream input(pageReferenceString);

        PageId page;
        long long currentTime = 0;

        long long hits = 0;
        long long misses = 0;

        while (input >> page) {

            ++currentTime;

            if (referencePage(page, currentTime)) {

                ++hits;

                } else {

                    ++misses;

                    }

            }


        if (!input.eof()) {

            throw invalid_argument(

            "The page-reference string contains an invalid value");

            }

        if (currentTime == 0) {

            throw invalid_argument(

            "The page-reference string is empty");

            }

        const double hitRatio =

        static_cast<double>(hits) /

        static_cast<double>(currentTime);

        cout << fixed << setprecision(4);

        cout << "\nResults\n";

        cout << "-------\n";

        cout << "Buffer size: " << BUFFER_SIZE << '\n';

        cout << "References: " << currentTime << '\n';

        cout << "Hits: " << hits << '\n';

        cout << "Misses: " << misses << '\n';

        cout << "Overall hit ratio: " << hitRatio << '\n';

        cout << "Final buffer: { ";

        for (PageId residentPage : bufferPool) {

            cout << residentPage << ' ';

            }

        cout << "}\n";

        return 0;

        } catch (const exception &error) {

            cerr << "Error: " << error.what() << '\n';

            return 1;

            }

    }
