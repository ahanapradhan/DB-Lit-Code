// Compact fixed version: Student table + disk B+ tree index on rollNumber
// Includes deletion with borrow + merge + root shrinking.
// Compile: g++ -std=c++17 -O2 -Wall -Wextra -o bpt bplustree_student_fixed.cpp
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace std;

#pragma pack(push, 1)
struct Student { int rollNumber; char name[21]; char grade; char phone[11]; };
#pragma pack(pop)

Student makeStudent(int roll, string name, char grade, string phone) {
    Student s{}; s.rollNumber = roll; s.grade = grade;
    strncpy(s.name, name.c_str(), sizeof(s.name) - 1);
    strncpy(s.phone, phone.c_str(), sizeof(s.phone) - 1);
    return s;
}
void printStudent(const Student &s) {
    cout << "Roll=" << setw(5) << s.rollNumber << " Name=" << setw(20) << left << s.name
         << " Grade=" << s.grade << " Phone=" << s.phone << '\n';
}

class StudentFile {
    fstream f;
public:
    explicit StudentFile(const string &path) {
        ofstream(path, ios::app).close();
        f.open(path, ios::in | ios::out | ios::binary);
        if (!f) throw runtime_error("cannot open data file");
    }
    long append(const Student &s) {
        f.clear(); f.seekp(0, ios::end); long off = (long)f.tellp();
        f.write((const char*)&s, sizeof s); f.flush(); return off;
    }
    Student read(long off) { Student s{}; f.clear(); f.seekg(off); f.read((char*)&s, sizeof s); return s; }
    void write(long off, const Student &s) { f.clear(); f.seekp(off); f.write((const char*)&s, sizeof s); f.flush(); }
};

static const int ORDER = 4;
static const int MAX_KEYS = ORDER - 1;
static const int MIN_LEAF = (MAX_KEYS + 1) / 2;   // 2 when ORDER=4
static const int MIN_INTERNAL = (ORDER + 1) / 2 - 1; // min keys, 1 when ORDER=4

struct Node {
    bool isLeaf{};
    int numKeys{};
    int keys[MAX_KEYS + 1]{};    // +1 overflow slot for insert
    long child[ORDER + 1]{};     // internal children OR leaf record offsets
    long nextLeaf{-1};
    long self{-1};
};
struct Header { long root{-1}, nextBlock{0}, count{0}; };

class BPlusTree {
    fstream f; Header h; bool verbose = false;
    long reads = 0, writes = 0, splits = 0, merges = 0;

    long pos(long b) const { return sizeof(Header) + b * (long)sizeof(Node); }
    void saveHeader() { f.clear(); f.seekp(0); f.write((char*)&h, sizeof h); f.flush(); }
    void loadHeader() { f.clear(); f.seekg(0); f.read((char*)&h, sizeof h); }
    long alloc() { long b = h.nextBlock++; saveHeader(); return b; }

    Node readNode(long b) {
        ++reads; Node n{}; f.clear(); f.seekg(pos(b)); f.read((char*)&n, sizeof n);
        if (verbose) {
            cout << "  read block " << b << (n.isLeaf ? " leaf " : " internal ") << "[";
            for (int i = 0; i < n.numKeys; ++i) cout << n.keys[i] << (i + 1 < n.numKeys ? "," : "");
            cout << "]\n";
        }
        return n;
    }
    void writeNode(const Node &n) { ++writes; f.clear(); f.seekp(pos(n.self)); f.write((const char*)&n, sizeof n); f.flush(); }

    int upperChild(const Node &n, int key) const { int i = 0; while (i < n.numKeys && key >= n.keys[i]) ++i; return i; }
    int lowerKey(const Node &n, int key) const { int i = 0; while (i < n.numKeys && n.keys[i] < key) ++i; return i; }

    int firstKey(long b) {
        Node n = readNode(b);
        while (!n.isLeaf) n = readNode(n.child[0]);
        return n.keys[0];
    }
    void recomputeKeys(Node &p) {
        for (int i = 0; i < p.numKeys; ++i) p.keys[i] = firstKey(p.child[i + 1]);
    }
    void removeChild(Node &p, int idx) {
        int oldChildren = p.numKeys + 1;
        for (int i = idx; i + 1 < oldChildren; ++i) p.child[i] = p.child[i + 1];
        --p.numKeys;
        if (p.numKeys > 0) recomputeKeys(p);
    }

    bool insertRec(long block, int key, long off, int &promoKey, long &promoBlock) {
        Node n = readNode(block);
        if (n.isLeaf) {
            int i = lowerKey(n, key);
            if (i < n.numKeys && n.keys[i] == key) { n.child[i] = off; writeNode(n); return false; }
            for (int j = n.numKeys; j > i; --j) { n.keys[j] = n.keys[j-1]; n.child[j] = n.child[j-1]; }
            n.keys[i] = key; n.child[i] = off; ++n.numKeys;
            if (n.numKeys <= MAX_KEYS) { writeNode(n); return false; }

            ++splits; Node r{}; r.isLeaf = true; r.self = alloc();
            int mid = n.numKeys / 2; r.numKeys = n.numKeys - mid;
            for (int j = 0; j < r.numKeys; ++j) { r.keys[j] = n.keys[mid+j]; r.child[j] = n.child[mid+j]; }
            n.numKeys = mid; r.nextLeaf = n.nextLeaf; n.nextLeaf = r.self;
            promoKey = r.keys[0]; promoBlock = r.self; writeNode(n); writeNode(r); return true;
        }

        int ci = upperChild(n, key), ck; long cb;
        if (!insertRec(n.child[ci], key, off, ck, cb)) return false;
        int i = lowerKey(n, ck);
        for (int j = n.numKeys; j > i; --j) n.keys[j] = n.keys[j-1];
        for (int j = n.numKeys + 1; j > i + 1; --j) n.child[j] = n.child[j-1];
        n.keys[i] = ck; n.child[i+1] = cb; ++n.numKeys;
        if (n.numKeys <= MAX_KEYS) { writeNode(n); return false; }

        ++splits; Node r{}; r.isLeaf = false; r.self = alloc();
        int mid = n.numKeys / 2; promoKey = n.keys[mid];
        r.numKeys = n.numKeys - mid - 1;
        for (int j = 0; j < r.numKeys; ++j) r.keys[j] = n.keys[mid+1+j];
        for (int j = 0; j <= r.numKeys; ++j) r.child[j] = n.child[mid+1+j];
        n.numKeys = mid; promoBlock = r.self; writeNode(n); writeNode(r); return true;
    }

    long firstLeaf() {
        if (h.root == -1) return -1;
        Node n = readNode(h.root); while (!n.isLeaf) n = readNode(n.child[0]); return n.self;
    }

    void fixUnderflow(long curBlock, vector<pair<long,int>> path) {
        while (!path.empty()) {
            auto [parentBlock, idx] = path.back(); path.pop_back();
            Node p = readNode(parentBlock), x = readNode(curBlock);
            bool xIsLeaf = x.isLeaf;
            int minKeys = xIsLeaf ? MIN_LEAF : MIN_INTERNAL;
            if (x.numKeys >= minKeys) { recomputeKeys(p); writeNode(p); return; }

            bool hasLeft = idx > 0, hasRight = idx < p.numKeys;
            Node left{}, right{};
            if (hasLeft) left = readNode(p.child[idx - 1]);
            if (hasRight) right = readNode(p.child[idx + 1]);

            // 1) Borrow from left sibling.
            if (hasLeft && left.numKeys > minKeys) {
                if (xIsLeaf) {
                    for (int i = x.numKeys; i > 0; --i) { x.keys[i] = x.keys[i-1]; x.child[i] = x.child[i-1]; }
                    x.keys[0] = left.keys[left.numKeys-1]; x.child[0] = left.child[left.numKeys-1];
                    --left.numKeys; ++x.numKeys;
                } else {
                    for (int i = x.numKeys + 1; i > 0; --i) x.child[i] = x.child[i-1];
                    x.child[0] = left.child[left.numKeys];
                    --left.numKeys; ++x.numKeys;
                    recomputeKeys(left); recomputeKeys(x);
                }
                recomputeKeys(p); writeNode(left); writeNode(x); writeNode(p); return;
            }

            // 2) Borrow from right sibling.
            if (hasRight && right.numKeys > minKeys) {
                if (xIsLeaf) {
                    x.keys[x.numKeys] = right.keys[0]; x.child[x.numKeys] = right.child[0];
                    ++x.numKeys;
                    for (int i = 0; i + 1 < right.numKeys; ++i) { right.keys[i] = right.keys[i+1]; right.child[i] = right.child[i+1]; }
                    --right.numKeys;
                } else {
                    x.child[x.numKeys + 1] = right.child[0]; ++x.numKeys;
                    for (int i = 0; i < right.numKeys; ++i) right.child[i] = right.child[i+1];
                    --right.numKeys;
                    recomputeKeys(x); recomputeKeys(right);
                }
                recomputeKeys(p); writeNode(x); writeNode(right); writeNode(p); return;
            }

            // 3) Merge. Prefer left merge; otherwise merge right into x.
            ++merges;
            if (hasLeft) {
                if (xIsLeaf) {
                    for (int i = 0; i < x.numKeys; ++i) {
                        left.keys[left.numKeys+i] = x.keys[i]; left.child[left.numKeys+i] = x.child[i];
                    }
                    left.numKeys += x.numKeys; left.nextLeaf = x.nextLeaf;
                } else {
                    int base = left.numKeys + 1;
                    for (int i = 0; i <= x.numKeys; ++i) left.child[base+i] = x.child[i];
                    left.numKeys += x.numKeys + 1; recomputeKeys(left);
                }
                removeChild(p, idx); writeNode(left); writeNode(p);
                curBlock = p.self;
            } else if (hasRight) {
                if (xIsLeaf) {
                    for (int i = 0; i < right.numKeys; ++i) {
                        x.keys[x.numKeys+i] = right.keys[i]; x.child[x.numKeys+i] = right.child[i];
                    }
                    x.numKeys += right.numKeys; x.nextLeaf = right.nextLeaf;
                } else {
                    int base = x.numKeys + 1;
                    for (int i = 0; i <= right.numKeys; ++i) x.child[base+i] = right.child[i];
                    x.numKeys += right.numKeys + 1; recomputeKeys(x);
                }
                removeChild(p, idx + 1); writeNode(x); writeNode(p);
                curBlock = p.self;
            }

            // Root can have fewer keys; shrink it if it has a single child.
            if (p.self == h.root) {
                p = readNode(p.self);
                if (!p.isLeaf && p.numKeys == 0) { h.root = p.child[0]; saveHeader(); }
                return;
            }
        }
    }

public:
    explicit BPlusTree(const string &path) {
        bool exists = ifstream(path, ios::binary).good();
        if (!exists) ofstream(path, ios::binary).close();
        f.open(path, ios::in | ios::out | ios::binary);
        if (!f) throw runtime_error("cannot open index file");
        if (!exists || f.peek() == ifstream::traits_type::eof()) saveHeader(); else loadHeader();
    }

    void setVerbose(bool v) { verbose = v; }
    void resetStats() { reads = writes = 0; }
    long diskReads() const { return reads; }
    long diskWrites() const { return writes; }
    long splitCount() const { return splits; }
    long mergeCount() const { return merges; }
    long recordCount() const { return h.count; }
    long nodeCount() const { return h.nextBlock; }

    int height() {
        if (h.root == -1) return 0;
        int ht = 1; Node n = readNode(h.root);
        while (!n.isLeaf) { n = readNode(n.child[0]); ++ht; }
        return ht;
    }

    void insert(int key, long off) {
        if (h.root == -1) {
            Node root{}; root.isLeaf = true; root.self = alloc(); root.numKeys = 1;
            root.keys[0] = key; root.child[0] = off; h.root = root.self; h.count = 1;
            saveHeader(); writeNode(root); return;
        }
        bool isNewKey = (search(key) == -1);
        int pk; long pb;
        if (insertRec(h.root, key, off, pk, pb)) {
            Node root{}; root.isLeaf = false; root.self = alloc(); root.numKeys = 1;
            root.keys[0] = pk; root.child[0] = h.root; root.child[1] = pb;
            h.root = root.self; writeNode(root);
        }
        if (isNewKey) ++h.count;
        saveHeader();
    }

    long search(int key) {
        if (h.root == -1) return -1;
        Node n = readNode(h.root);
        while (!n.isLeaf) n = readNode(n.child[upperChild(n, key)]);
        int i = lowerKey(n, key);
        return (i < n.numKeys && n.keys[i] == key) ? n.child[i] : -1;
    }

    vector<long> rangeSearch(int lo, int hi) {
        vector<long> out;
        if (h.root == -1 || lo > hi) return out;
        Node n = readNode(h.root);
        while (!n.isLeaf) n = readNode(n.child[upperChild(n, lo)]);
        while (true) {
            for (int i = 0; i < n.numKeys; ++i) {
                if (n.keys[i] > hi) return out;
                if (n.keys[i] >= lo) out.push_back(n.child[i]);
            }
            if (n.nextLeaf == -1) return out;
            n = readNode(n.nextLeaf);
        }
    }

    vector<int> scanAllKeys() {
        vector<int> keys;
        for (long b = firstLeaf(); b != -1; ) {
            Node n = readNode(b);
            for (int i = 0; i < n.numKeys; ++i) keys.push_back(n.keys[i]);
            b = n.nextLeaf;
        }
        return keys;
    }

    bool erase(int key) {
        if (h.root == -1) return false;

        vector<pair<long,int>> path;          // parent block + chosen child index
        Node n = readNode(h.root);
        while (!n.isLeaf) {
            int ci = upperChild(n, key);
            path.push_back({n.self, ci});
            n = readNode(n.child[ci]);
        }

        int i = lowerKey(n, key);
        if (i == n.numKeys || n.keys[i] != key) return false;
        for (int j = i; j + 1 < n.numKeys; ++j) { n.keys[j] = n.keys[j+1]; n.child[j] = n.child[j+1]; }
        --n.numKeys; --h.count; saveHeader(); writeNode(n);

        if (n.self == h.root) {              // root leaf may become empty
            if (n.numKeys == 0) { h.root = -1; saveHeader(); }
            return true;
        }
        if (n.numKeys < MIN_LEAF) fixUnderflow(n.self, path);
        else if (!path.empty()) {             // first key may have changed; refresh parent separators
            Node p = readNode(path.back().first); recomputeKeys(p); writeNode(p);
        }
        return true;
    }
};

int main() {
    const string DATA = "students.dat", INDEX = "index.dat";
    remove(DATA.c_str()); remove(INDEX.c_str());
    StudentFile data(DATA); BPlusTree tree(INDEX);

    vector<int> rolls;
    for (int i = 1; i <= 100; ++i) rolls.push_back(1000 + i);
    mt19937 rng(42); shuffle(rolls.begin(), rolls.end(), rng);

    for (int r : rolls) {
        long off = data.append(makeStudent(r, "Student" + to_string(r), char('A' + r % 5), "9999999999"));
        tree.insert(r, off);
    }

    cout << "Inserted " << tree.recordCount() << " records. Height=" << tree.height()
         << " Nodes=" << tree.nodeCount() << "\n";
    auto keys = tree.scanAllKeys();
    cout << "Scan sorted? " << boolalpha << is_sorted(keys.begin(), keys.end())
         << ", first=" << keys.front() << ", last=" << keys.back() << "\n";

    cout << "\nPoint read 1042:\n";
    long off = tree.search(1042); if (off != -1) printStudent(data.read(off));

    cout << "\nRange read 1040..1045:\n";
    for (long x : tree.rangeSearch(1040, 1045)) printStudent(data.read(x));

    cout << "\nUpdate non-key field for 1042:\n";
    off = tree.search(1042);
    if (off != -1) { Student s = data.read(off); s.grade = 'S'; data.write(off, s); printStudent(data.read(off)); }

    cout << "\nUpdate key 1042 -> 2042:\n";
    off = tree.search(1042);
    if (off != -1 && tree.erase(1042)) {
        Student s = data.read(off); s.rollNumber = 2042; data.write(off, s); tree.insert(2042, off);
    }
    cout << "search 1042 = " << tree.search(1042) << ", search 2042 = " << tree.search(2042) << "\n";

    cout << "\nDelete many keys to force merges:\n";
    for (int r = 1001; r <= 1070; ++r) tree.erase(r);
    keys = tree.scanAllKeys();
    cout << "Remaining=" << tree.recordCount() << " Height=" << tree.height()
         << " Merges=" << tree.mergeCount() << " Sorted=" << is_sorted(keys.begin(), keys.end()) << "\n";
}
