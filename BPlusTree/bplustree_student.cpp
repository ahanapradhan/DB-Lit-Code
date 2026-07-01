/*
 * ============================================================================
 *  STUDENT TABLE  +  DISK-BASED B+ TREE INDEX ON ROLL NUMBER
 * ============================================================================
 *
 * Relational schema:
 *
 *      Student ( RollNumber INT PRIMARY KEY,
 *                Name       VARCHAR(20),
 *                Grade      CHAR(1),
 *                PhoneNo    CHAR(10) )        -- 10-digit phone number
 *
 * Two files on disk are used, exactly like a real DBMS separates the
 * data (heap) file from the index file:
 *
 *      students.dat   -> heap file, one fixed-size Student record per slot,
 *                         records are simply appended in insertion order.
 *
 *      index.dat      -> the B+ Tree index file. Every node of the tree is
 *                         a fixed-size "block" written at a fixed offset
 *                         (blockNumber * sizeof(Node) + header size), the
 *                         same way a DBMS stores pages of an index in a
 *                         .ibd / .myi / .idx file. Block 0 area holds a
 *                         Header record (root block number, next free
 *                         block, record count) so the tree can be re-opened
 *                         later without rebuilding it.
 *
 *  Leaf nodes store (RollNumber -> offset in students.dat) and are linked
 *  together (nextLeaf) so that range queries can be answered by locating
 *  the first qualifying leaf and then following sibling pointers, without
 *  ever going back up to the root - exactly how B+ Tree range scans work
 *  in production databases.
 *
 *  Every disk block read, every routing decision made while walking down
 *  the tree, and every node split is printed to stdout so you can watch
 *  the traversal happen.
 *
 *  Compile:   g++ -std=c++17 -O2 -o student_bplustree student_bplustree.cpp
 *  Run:       ./student_bplustree
 * ============================================================================
 */

#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <algorithm>
#include <random>
#include <iomanip>
#include <string>
#include <sstream>
#include <cstdio>

using namespace std;

/* ============================================================================
 * 1. STUDENT RECORD  (maps the SQL schema onto a plain, fixed-size C++ struct
 *    so it can be written/read to disk with a raw fwrite/fread of sizeof(Student)
 *    bytes - no serialization library needed).
 * ============================================================================
 */
#pragma pack(push, 1)
struct Student {
    int  rollNumber;   // INT
    char name[21];     // VARCHAR(20) + NUL terminator
    char grade;        // CHAR(1)
    char phone[11];    // 10-digit phone number, stored as digit string + NUL
};
#pragma pack(pop)

static Student makeStudent(int roll, const string &nm, char grade, const string &ph) {
    Student s{};
    s.rollNumber = roll;
    memset(s.name, 0, sizeof(s.name));
    strncpy(s.name, nm.c_str(), sizeof(s.name) - 1);
    s.grade = grade;
    memset(s.phone, 0, sizeof(s.phone));
    strncpy(s.phone, ph.c_str(), sizeof(s.phone) - 1);
    return s;
}

static void printStudent(const Student &s) {
    cout << "    RollNo=" << setw(5) << left << s.rollNumber
         << " Name=" << setw(21) << left << s.name
         << " Grade=" << s.grade
         << " Phone=" << s.phone << "\n";
}

/* ============================================================================
 * 2. HEAP DATA FILE (students.dat)
 *    Records are simply appended. The B+ Tree never stores the record
 *    itself in a leaf - only the byte offset where the record lives here.
 *    This is the classic "unclustered index" layout.
 * ============================================================================
 */
class StudentDataFile {
    fstream file;
    string filename;
public:
    explicit StudentDataFile(const string &fname) : filename(fname) {
        // create the file if it does not already exist
        { ofstream touch(fname, ios::app); }
        file.open(fname, ios::in | ios::out | ios::binary);
        if (!file.is_open()) {
            cerr << "FATAL: could not open data file " << fname << "\n";
            exit(1);
        }
    }

    // append a record, return its byte offset in the file
    long appendRecord(const Student &s) {
        file.clear();
        file.seekp(0, ios::end);
        long offset = static_cast<long>(file.tellp());
        file.write(reinterpret_cast<const char *>(&s), sizeof(Student));
        file.flush();
        return offset;
    }

    Student readRecord(long offset) {
        Student s{};
        file.clear();
        file.seekg(offset, ios::beg);
        file.read(reinterpret_cast<char *>(&s), sizeof(Student));
        return s;
    }

    ~StudentDataFile() { if (file.is_open()) file.close(); }
};

/* ============================================================================
 * 3. B+ TREE NODE  -  fixed-size disk "block"
 *
 *    ORDER = 4  =>  each node holds at most 3 keys / 4 children.
 *    A small order is used deliberately so that inserting only 100 records
 *    already produces several splits and a multi-level tree, which makes
 *    the traversal logs interesting to watch. (In a real database ORDER
 *    would be in the hundreds, sized to match the disk page size.)
 *
 *    Every array is declared ONE element larger than the true maximum
 *    (MAX_KEYS+1 / ORDER+1). This lets us insert first and then check for
 *    overflow, which is the standard, simplest way to implement B+ Tree
 *    insertion.
 * ============================================================================
 */
static const int ORDER      = 4;              // max children per internal node
static const int MAX_KEYS   = ORDER - 1;       // max keys per node (3)

struct Node {
    bool isLeaf;
    int  numKeys;
    int  keys[MAX_KEYS + 1];                   // +1 = temporary overflow slot
    long children[ORDER + 1];                  // internal node: child block numbers
    long recordOffset[MAX_KEYS + 1];            // leaf node: offset into students.dat
    long nextLeaf;                              // leaf node: sibling block number, -1 = none
    long selfBlock;                             // this node's own block number
};

// header block: block 0 area of index.dat
struct IndexHeader {
    long rootBlock;
    long nextFreeBlock;
    long recordCount;
};

/* ============================================================================
 * 4. DISK-BASED B+ TREE
 *    All node reads/writes go through readNode()/writeNode(), which do a
 *    real fseek+fread / fseek+fwrite against index.dat, so the tree is
 *    genuinely persisted on disk between reads (nothing about the tree
 *    structure is cached in memory beyond the header + whatever node is
 *    currently being examined).
 * ============================================================================
 */
class DiskBPlusTree {
    fstream indexFile;
    string  filename;
    IndexHeader header{};
    bool verbose = false;

    long blockOffset(long blockNum) const {
        return sizeof(IndexHeader) + blockNum * static_cast<long>(sizeof(Node));
    }

    void writeHeader() {
        indexFile.clear();
        indexFile.seekp(0, ios::beg);
        indexFile.write(reinterpret_cast<char *>(&header), sizeof(IndexHeader));
        indexFile.flush();
    }

    void readHeader() {
        indexFile.clear();
        indexFile.seekg(0, ios::beg);
        indexFile.read(reinterpret_cast<char *>(&header), sizeof(IndexHeader));
    }

    long allocateBlock() {
        long b = header.nextFreeBlock++;
        writeHeader();
        return b;
    }

    Node readNode(long blockNum) {
        Node n{};
        indexFile.clear();
        indexFile.seekg(blockOffset(blockNum), ios::beg);
        indexFile.read(reinterpret_cast<char *>(&n), sizeof(Node));
        if (verbose) {
            cout << "    [DISK READ] block " << blockNum
                 << " (" << (n.isLeaf ? "LEAF" : "INTERNAL") << ")"
                 << "  keys=[";
            for (int i = 0; i < n.numKeys; i++) cout << n.keys[i] << (i + 1 < n.numKeys ? "," : "");
            cout << "]" << (n.isLeaf ? ("  nextLeaf=" + to_string(n.nextLeaf)) : "") << "\n";
        }
        return n;
    }

    void writeNode(const Node &n) {
        indexFile.clear();
        indexFile.seekp(blockOffset(n.selfBlock), ios::beg);
        indexFile.write(reinterpret_cast<const char *>(&n), sizeof(Node));
        indexFile.flush();
    }

    // Recursive insert helper.
    // Returns true if this call caused a split; on split, promotedKey /
    // newBlock describe the new right-hand sibling that must be linked
    // into the parent.
    bool insertRec(long nodeBlock, int key, long recOffset, int &promotedKey, long &newBlock) {
        Node node = readNode(nodeBlock);

        if (node.isLeaf) {
            if (verbose) cout << "    [TRAVERSE] leaf block " << nodeBlock << " -> inserting key " << key << "\n";

            int pos = 0;
            while (pos < node.numKeys && node.keys[pos] < key) pos++;
            for (int i = node.numKeys; i > pos; i--) {
                node.keys[i]         = node.keys[i - 1];
                node.recordOffset[i] = node.recordOffset[i - 1];
            }
            node.keys[pos]         = key;
            node.recordOffset[pos] = recOffset;
            node.numKeys++;

            if (node.numKeys <= MAX_KEYS) {
                writeNode(node);
                return false;
            }

            // --- LEAF SPLIT ---
            if (verbose) cout << "    [SPLIT] leaf block " << nodeBlock << " overflowed (" << node.numKeys
                               << " keys) -> splitting\n";
            long rightBlock = allocateBlock();
            Node right{};
            right.isLeaf   = true;
            right.selfBlock = rightBlock;

            int mid        = (node.numKeys + 1) / 2;   // e.g. 4 keys -> mid = 2
            int rightCount = node.numKeys - mid;
            right.numKeys  = rightCount;
            for (int i = 0; i < rightCount; i++) {
                right.keys[i]         = node.keys[mid + i];
                right.recordOffset[i] = node.recordOffset[mid + i];
            }
            right.nextLeaf = node.nextLeaf;
            node.nextLeaf  = rightBlock;
            node.numKeys   = mid;

            writeNode(node);
            writeNode(right);

            promotedKey = right.keys[0];
            newBlock    = rightBlock;
            if (verbose) cout << "    [SPLIT] new leaf block " << rightBlock
                               << " created, promoted key=" << promotedKey << "\n";
            return true;

        } else {
            if (verbose) cout << "    [TRAVERSE] internal block " << nodeBlock << "\n";

            int pos = 0;
            while (pos < node.numKeys && key >= node.keys[pos]) pos++;
            if (verbose) cout << "        key " << key << " routes to child #" << pos
                               << " (block " << node.children[pos] << ")\n";

            int  childPromotedKey;
            long childNewBlock;
            bool childSplit = insertRec(node.children[pos], key, recOffset, childPromotedKey, childNewBlock);

            if (!childSplit) return false; // child updated itself on disk already, nothing to do here

            // insert (childPromotedKey, childNewBlock) into this node
            for (int i = node.numKeys; i > pos; i--) node.keys[i] = node.keys[i - 1];
            for (int i = node.numKeys + 1; i > pos + 1; i--) node.children[i] = node.children[i - 1];
            node.keys[pos]       = childPromotedKey;
            node.children[pos+1] = childNewBlock;
            node.numKeys++;

            if (node.numKeys <= MAX_KEYS) {
                writeNode(node);
                return false;
            }

            // --- INTERNAL SPLIT ---
            if (verbose) cout << "    [SPLIT] internal block " << nodeBlock << " overflowed (" << node.numKeys
                               << " keys) -> splitting\n";
            long rightBlock = allocateBlock();
            Node right{};
            right.isLeaf    = false;
            right.selfBlock = rightBlock;

            int mid = node.numKeys / 2;
            promotedKey = node.keys[mid];              // middle key is pushed UP, not copied

            int rightCount = node.numKeys - mid - 1;
            right.numKeys  = rightCount;
            for (int i = 0; i < rightCount; i++) right.keys[i] = node.keys[mid + 1 + i];
            for (int i = 0; i <= rightCount; i++) right.children[i] = node.children[mid + 1 + i];

            node.numKeys = mid;

            writeNode(node);
            writeNode(right);

            newBlock = rightBlock;
            if (verbose) cout << "    [SPLIT] new internal block " << rightBlock
                               << " created, promoted key=" << promotedKey << "\n";
            return true;
        }
    }

public:
    explicit DiskBPlusTree(const string &fname) : filename(fname) {
        bool exists = ifstream(fname).good();
        if (!exists) {
            indexFile.open(fname, ios::out | ios::binary);
            indexFile.close();
            indexFile.open(fname, ios::in | ios::out | ios::binary);
            header.rootBlock     = -1;
            header.nextFreeBlock = 0;
            header.recordCount   = 0;
            writeHeader();
        } else {
            indexFile.open(fname, ios::in | ios::out | ios::binary);
            readHeader();
        }
    }

    void setVerbose(bool v) { verbose = v; }

    long recordCount() const { return header.recordCount; }

    // Height of the tree, purely for reporting stats to the user.
    int height() {
        if (header.rootBlock == -1) return 0;
        int h = 1;
        long cur = header.rootBlock;
        while (true) {
            Node n = readNode(cur);
            if (n.isLeaf) break;
            cur = n.children[0];
            h++;
        }
        return h;
    }

    void insert(int key, long recOffset) {
        if (verbose) cout << "  >> INSERT roll=" << key << "\n";

        if (header.rootBlock == -1) {
            long b = allocateBlock();
            Node root{};
            root.isLeaf     = true;
            root.selfBlock  = b;
            root.numKeys    = 1;
            root.keys[0]    = key;
            root.recordOffset[0] = recOffset;
            root.nextLeaf   = -1;
            writeNode(root);
            header.rootBlock = b;
            header.recordCount++;
            writeHeader();
            if (verbose) cout << "    [ROOT] created initial root leaf at block " << b << "\n";
            return;
        }

        int  promotedKey;
        long newBlock;
        bool split = insertRec(header.rootBlock, key, recOffset, promotedKey, newBlock);

        if (split) {
            long newRootBlock = allocateBlock();
            Node newRoot{};
            newRoot.isLeaf      = false;
            newRoot.selfBlock   = newRootBlock;
            newRoot.numKeys     = 1;
            newRoot.keys[0]     = promotedKey;
            newRoot.children[0] = header.rootBlock;
            newRoot.children[1] = newBlock;
            writeNode(newRoot);
            header.rootBlock = newRootBlock;
            if (verbose) cout << "    [ROOT] split propagated to the top; new root block " << newRootBlock << "\n";
        }

        header.recordCount++;
        writeHeader();
    }

    // Point search: returns offset into students.dat, or -1 if not found.
    long search(int key) {
        if (header.rootBlock == -1) return -1;
        if (verbose) cout << "[SEARCH] looking for RollNumber=" << key << "\n";

        long cur = header.rootBlock;
        while (true) {
            Node node = readNode(cur);
            if (node.isLeaf) {
                for (int i = 0; i < node.numKeys; i++) {
                    if (node.keys[i] == key) {
                        if (verbose) cout << "    [FOUND] key " << key << " in leaf block " << cur
                                           << " at slot " << i << "\n";
                        return node.recordOffset[i];
                    }
                }
                if (verbose) cout << "    [NOT FOUND] key " << key << " is absent from leaf block " << cur << "\n";
                return -1;
            }
            int pos = 0;
            while (pos < node.numKeys && key >= node.keys[pos]) pos++;
            if (verbose) cout << "    routing to child #" << pos << " (block " << node.children[pos] << ")\n";
            cur = node.children[pos];
        }
    }

    // Range search [lowKey, highKey]: descend once to the first qualifying
    // leaf, then walk the leaf-sibling chain - the classic B+ Tree range
    // scan, touching each relevant leaf exactly once and never revisiting
    // internal nodes.
    vector<long> rangeSearch(int lowKey, int highKey) {
        vector<long> results;
        if (header.rootBlock == -1) return results;
        if (verbose) cout << "[RANGE SEARCH] RollNumber in [" << lowKey << ", " << highKey << "]\n";

        long cur = header.rootBlock;
        while (true) {
            Node node = readNode(cur);
            if (node.isLeaf) break;
            int pos = 0;
            while (pos < node.numKeys && lowKey >= node.keys[pos]) pos++;
            if (verbose) cout << "    descending to child #" << pos << " (block " << node.children[pos] << ")\n";
            cur = node.children[pos];
        }

        if (verbose) cout << "    reached starting leaf, now scanning leaf chain via nextLeaf pointers...\n";
        while (cur != -1) {
            Node node = readNode(cur);
            bool exceeded = false;
            for (int i = 0; i < node.numKeys; i++) {
                if (node.keys[i] > highKey) { exceeded = true; break; }
                if (node.keys[i] >= lowKey) {
                    results.push_back(node.recordOffset[i]);
                    if (verbose) cout << "        [MATCH] key " << node.keys[i] << "\n";
                }
            }
            if (exceeded) {
                if (verbose) cout << "    [STOP] passed upper bound " << highKey << ", ending scan\n";
                break;
            }
            if (node.nextLeaf != -1 && verbose)
                cout << "    [NEXT LEAF] following sibling pointer to block " << node.nextLeaf << "\n";
            cur = node.nextLeaf;
        }
        return results;
    }

    // Full in-order scan of every key in the tree (leftmost leaf -> follow
    // nextLeaf to the end). Useful to prove the index is really sorted.
    vector<int> scanAllKeys() {
        vector<int> all;
        if (header.rootBlock == -1) return all;
        long cur = header.rootBlock;
        while (true) {
            Node n = readNode(cur);
            if (n.isLeaf) break;
            cur = n.children[0];
        }
        while (cur != -1) {
            Node n = readNode(cur);
            for (int i = 0; i < n.numKeys; i++) all.push_back(n.keys[i]);
            cur = n.nextLeaf;
        }
        return all;
    }

    ~DiskBPlusTree() { if (indexFile.is_open()) indexFile.close(); }
};

/* ============================================================================
 * 5. TEST / DEMO DRIVER
 * ============================================================================
 */
int main() {
    const string DATA_FILE  = "students.dat";
    const string INDEX_FILE = "index.dat";

    // Start each demo run from a clean pair of files, the same way you'd
    // drop and recreate a table + its index before a repeatable test.
    remove(DATA_FILE.c_str());
    remove(INDEX_FILE.c_str());

    StudentDataFile dataFile(DATA_FILE);
    DiskBPlusTree   tree(INDEX_FILE);

    // ---- generate 100 sample students -------------------------------------
    static const char *firstNames[] = {
        "Aarav","Isha","Rohan","Meera","Kabir","Sara","Vikram","Anaya","Dev","Priya",
        "Arjun","Neha","Kunal","Divya","Rahul","Tara","Aman","Riya","Sahil","Pooja"
    };
    static const char *lastNames[] = {
        "Sharma","Verma","Iyer","Nair","Gupta","Rao","Mehta","Joshi","Reddy","Kapoor"
    };
    static const char grades[] = {'A','B','C','D','F'};

    mt19937 rng(42); // fixed seed -> reproducible demo run
    uniform_int_distribution<int> firstIdx(0, 19);
    uniform_int_distribution<int> lastIdx(0, 9);
    uniform_int_distribution<int> gradeIdx(0, 4);
    uniform_int_distribution<int> digit(0, 9);

    struct Row { int roll; string name; char grade; string phone; };
    vector<Row> rows;
    for (int i = 1; i <= 100; i++) {
        int roll = 1000 + i;                       // roll numbers 1001..1100
        string name = string(firstNames[firstIdx(rng)]) + " " + lastNames[lastIdx(rng)];
        if (name.size() > 20) name = name.substr(0, 20);
        char grade = grades[gradeIdx(rng)];
        string phone = "9";
        for (int d = 0; d < 9; d++) phone += char('0' + digit(rng));
        rows.push_back({roll, name, grade, phone});
    }
    // insert in a shuffled (non-sequential) order, like real-world inserts
    shuffle(rows.begin(), rows.end(), rng);

    cout << "================================================================\n";
    cout << " STEP 1: Inserting 100 Student records\n";
    cout << " (data file: " << DATA_FILE << "   index file: " << INDEX_FILE << ")\n";
    cout << "================================================================\n";

    for (int i = 0; i < (int)rows.size(); i++) {
        const Row &r = rows[i];
        Student s = makeStudent(r.roll, r.name, r.grade, r.phone);
        long offset = dataFile.appendRecord(s);

        // Show FULL traversal/split logging for the first 5 inserts so you
        // can see exactly how the tree is built block by block; after that
        // switch to a concise one-line-per-insert summary so the output
        // for the remaining 95 inserts stays readable.
        bool showFull = (i < 5);
        tree.setVerbose(showFull);
        tree.insert(r.roll, offset);
        if (!showFull) {
            cout << "  insert #" << (i + 1) << ": roll=" << r.roll
                 << "  \"" << r.name << "\"  -> data offset " << offset << "\n";
        }
        cout << (showFull ? "  ------------------------------------------------\n" : "");
    }
    tree.setVerbose(false);

    cout << "\nAll 100 records inserted.\n";
    cout << "Index now has " << tree.recordCount() << " entries, tree height = "
         << tree.height() << " levels.\n";

    // ---- verify the index is really sorted on disk -------------------------
    cout << "\n================================================================\n";
    cout << " STEP 2: Verify - full in-order index scan (leaf chain traversal)\n";
    cout << "================================================================\n";
    vector<int> allKeys = tree.scanAllKeys();
    bool sorted = is_sorted(allKeys.begin(), allKeys.end());
    cout << "Total keys found by walking the leaf chain: " << allKeys.size() << "\n";
    cout << "Keys are " << (sorted ? "CORRECTLY SORTED" : "*** NOT SORTED - BUG ***") << "\n";
    cout << "First 10 keys: ";
    for (int i = 0; i < 10 && i < (int)allKeys.size(); i++) cout << allKeys[i] << " ";
    cout << "\nLast 10 keys:  ";
    for (int i = max(0, (int)allKeys.size() - 10); i < (int)allKeys.size(); i++) cout << allKeys[i] << " ";
    cout << "\n";

    // ---- point search demo, with full traversal logging --------------------
    cout << "\n================================================================\n";
    cout << " STEP 3: Point search demo (full traversal log)\n";
    cout << "================================================================\n";
    tree.setVerbose(true);

    int foundKey = rows[70].roll;   // a key we know is present
    cout << "\n-- Searching for an existing RollNumber " << foundKey << " --\n";
    long off = tree.search(foundKey);
    if (off != -1) {
        Student s = dataFile.readRecord(off);
        cout << "  Record retrieved from data file at offset " << off << ":\n";
        printStudent(s);
    } else {
        cout << "  Not found (unexpected!).\n";
    }

    int missingKey = 9999;
    cout << "\n-- Searching for a RollNumber that does not exist (" << missingKey << ") --\n";
    off = tree.search(missingKey);
    cout << "  Result: " << (off == -1 ? "NOT FOUND (as expected)" : "found unexpectedly") << "\n";

    // ---- range search demo, with full traversal logging ---------------------
    cout << "\n================================================================\n";
    cout << " STEP 4: Range search demo, RollNumber BETWEEN 1040 AND 1060\n";
    cout << "================================================================\n";
    vector<long> hits = tree.rangeSearch(1040, 1060);
    cout << "\n" << hits.size() << " record(s) matched. Reading them from " << DATA_FILE << ":\n";
    for (long o : hits) {
        Student s = dataFile.readRecord(o);
        printStudent(s);
    }

    tree.setVerbose(false);

    cout << "\n================================================================\n";
    cout << " DONE. Persistent files on disk:\n";
    cout << "   " << DATA_FILE  << "  (heap file of Student records)\n";
    cout << "   " << INDEX_FILE << "  (B+ Tree index keyed on RollNumber)\n";
    cout << " Re-running this program overwrites both files with a fresh demo dataset.\n";
    cout << " To use the index across separate runs without rebuilding it, simply\n";
    cout << " remove the 'remove(...)' calls at the top of main() and re-open the\n";
    cout << " same two files - the header block preserves rootBlock / nextFreeBlock\n";
    cout << " so DiskBPlusTree(INDEX_FILE) picks up exactly where it left off.\n";
    cout << "================================================================\n";

    return 0;
}
