#include <iostream>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstdio>
#include <cassert>

// ===================== File-based Storage =====================

// A simple file-based storage manager
class FileManager {
    std::fstream file;
    std::string filename;
    int next_pos; // next available position in bytes

public:
    FileManager() : next_pos(0) {}

    void init(const std::string &fname) {
        filename = fname;
        file.open(fname, std::ios::in | std::ios::out | std::ios::binary);
        if (!file) {
            // Create file
            std::ofstream create(fname, std::ios::binary);
            next_pos = sizeof(int);
            create.write(reinterpret_cast<char*>(&next_pos), sizeof(int));
            create.close();
            file.open(fname, std::ios::in | std::ios::out | std::ios::binary);
        } else {
            file.read(reinterpret_cast<char*>(&next_pos), sizeof(int));
        }
    }

    ~FileManager() {
        if (file.is_open()) {
            file.seekp(0);
            file.write(reinterpret_cast<char*>(&next_pos), sizeof(int));
            file.close();
        }
    }

    // Allocate space and return position
    int alloc(int size) {
        int pos = next_pos;
        next_pos += size;
        file.seekp(0);
        file.write(reinterpret_cast<char*>(&next_pos), sizeof(int));
        return pos;
    }

    template<typename T>
    void read(int pos, T &val) {
        file.seekg(pos);
        file.read(reinterpret_cast<char*>(&val), sizeof(T));
    }

    template<typename T>
    void write(int pos, const T &val) {
        file.seekp(pos);
        file.write(reinterpret_cast<const char*>(&val), sizeof(T));
        file.flush();
    }

    void readRaw(int pos, void *buf, int size) {
        file.seekg(pos);
        file.read(reinterpret_cast<char*>(buf), size);
    }

    void writeRaw(int pos, const void *buf, int size) {
        file.seekp(pos);
        file.write(reinterpret_cast<const char*>(buf), size);
        file.flush();
    }
};

// ===================== BPlusTree (Unrolled Linked List on file) =====================

// We use an unrolled linked list (block list) stored on file.
// Key = fixed-size string pair (index_key, value_key), both up to KEY_LEN chars.

static const int BLOCK_SIZE = 300; // max elements per block
static const int KEY_LEN = 65;

struct Key {
    char index[KEY_LEN]; // the indexed field
    char value[KEY_LEN]; // the secondary key (e.g., UserID or ISBN)

    Key() { memset(index, 0, KEY_LEN); memset(value, 0, KEY_LEN); }
    Key(const std::string &idx, const std::string &val) {
        memset(index, 0, KEY_LEN);
        memset(value, 0, KEY_LEN);
        strncpy(index, idx.c_str(), KEY_LEN - 1);
        strncpy(value, val.c_str(), KEY_LEN - 1);
    }

    bool operator<(const Key &o) const {
        int c = strcmp(index, o.index);
        if (c != 0) return c < 0;
        return strcmp(value, o.value) < 0;
    }
    bool operator==(const Key &o) const {
        return strcmp(index, o.index) == 0 && strcmp(value, o.value) == 0;
    }
    bool operator<=(const Key &o) const { return !(o < *this); }
    bool operator>(const Key &o) const { return o < *this; }
    bool operator>=(const Key &o) const { return !(*this < o); }
    bool operator!=(const Key &o) const { return !(*this == o); }
};

struct BlockNode {
    int count;
    int next; // file position of next block, 0 if none
    Key keys[BLOCK_SIZE];
    int values[BLOCK_SIZE]; // associated int value (file position of record)

    BlockNode() : count(0), next(0) {}
};

class BlockList {
    FileManager fm;
    int head; // file position of head block, 0 if empty
    int head_pos; // position where head pointer is stored (after FileManager's next_pos)

public:
    void init(const std::string &filename) {
        fm.init(filename);
        // We store head at position sizeof(int) in the file
        // But FileManager already uses position 0 for next_pos
        // Let's store head inside first alloc'd space
        // Actually, let's check if file is new
        int file_next;
        fm.read(0, file_next);
        if (file_next == (int)sizeof(int)) {
            // New file, allocate space for head pointer
            head_pos = fm.alloc(sizeof(int));
            head = 0;
            fm.write(head_pos, head);
        } else {
            head_pos = sizeof(int);
            fm.read(head_pos, head);
        }
    }

    void insert(const Key &key, int val) {
        if (head == 0) {
            // Create first block
            BlockNode node;
            node.count = 1;
            node.next = 0;
            node.keys[0] = key;
            node.values[0] = val;
            head = fm.alloc(sizeof(BlockNode));
            fm.write(head, node);
            fm.write(head_pos, head);
            return;
        }

        // Find the right block
        int cur_pos = head;
        int prev_pos = 0;
        BlockNode cur;

        while (cur_pos != 0) {
            fm.read(cur_pos, cur);
            if (cur.next == 0) break; // last block, insert here
            if (cur.keys[cur.count - 1] >= key) break; // key fits in this block
            prev_pos = cur_pos;
            cur_pos = cur.next;
        }

        fm.read(cur_pos, cur);

        // Find insertion position within block
        int pos = 0;
        while (pos < cur.count && cur.keys[pos] < key) pos++;

        // Check for duplicate
        if (pos < cur.count && cur.keys[pos] == key && cur.values[pos] == val) return;

        // Shift elements
        for (int i = cur.count; i > pos; i--) {
            cur.keys[i] = cur.keys[i-1];
            cur.values[i] = cur.values[i-1];
        }
        cur.keys[pos] = key;
        cur.values[pos] = val;
        cur.count++;

        // Split if needed
        if (cur.count >= BLOCK_SIZE) {
            BlockNode new_node;
            int mid = cur.count / 2;
            new_node.count = cur.count - mid;
            for (int i = 0; i < new_node.count; i++) {
                new_node.keys[i] = cur.keys[mid + i];
                new_node.values[i] = cur.values[mid + i];
            }
            new_node.next = cur.next;
            cur.count = mid;

            int new_pos = fm.alloc(sizeof(BlockNode));
            cur.next = new_pos;
            fm.write(new_pos, new_node);
        }

        fm.write(cur_pos, cur);
    }

    void erase(const Key &key, int val) {
        if (head == 0) return;

        int cur_pos = head;
        int prev_pos = 0;
        BlockNode cur;

        while (cur_pos != 0) {
            fm.read(cur_pos, cur);
            if (cur.keys[cur.count - 1] >= key || cur.next == 0) break;
            prev_pos = cur_pos;
            cur_pos = cur.next;
        }

        fm.read(cur_pos, cur);

        // Find element
        int pos = -1;
        for (int i = 0; i < cur.count; i++) {
            if (cur.keys[i] == key && cur.values[i] == val) {
                pos = i;
                break;
            }
        }
        if (pos == -1) return;

        // Shift elements
        for (int i = pos; i < cur.count - 1; i++) {
            cur.keys[i] = cur.keys[i+1];
            cur.values[i] = cur.values[i+1];
        }
        cur.count--;

        // If block is empty and not the only block
        if (cur.count == 0 && (prev_pos != 0 || cur.next != 0)) {
            if (prev_pos == 0) {
                head = cur.next;
                fm.write(head_pos, head);
            } else {
                BlockNode prev;
                fm.read(prev_pos, prev);
                prev.next = cur.next;
                fm.write(prev_pos, prev);
            }
        } else {
            // Try to merge with next block
            if (cur.next != 0) {
                BlockNode next_node;
                fm.read(cur.next, next_node);
                if (cur.count + next_node.count < BLOCK_SIZE) {
                    for (int i = 0; i < next_node.count; i++) {
                        cur.keys[cur.count + i] = next_node.keys[i];
                        cur.values[cur.count + i] = next_node.values[i];
                    }
                    cur.count += next_node.count;
                    cur.next = next_node.next;
                }
            }
            fm.write(cur_pos, cur);
        }
    }

    // Find all values with matching index key
    std::vector<int> find(const std::string &index_key) {
        std::vector<int> result;
        if (head == 0) return result;

        Key low(index_key, "");
        int cur_pos = head;
        BlockNode cur;

        // Skip blocks that are entirely before our key
        while (cur_pos != 0) {
            fm.read(cur_pos, cur);
            if (cur.count > 0 && strcmp(cur.keys[cur.count-1].index, index_key.c_str()) >= 0) break;
            cur_pos = cur.next;
        }

        while (cur_pos != 0) {
            fm.read(cur_pos, cur);
            for (int i = 0; i < cur.count; i++) {
                int cmp = strcmp(cur.keys[i].index, index_key.c_str());
                if (cmp > 0) return result;
                if (cmp == 0) result.push_back(cur.values[i]);
            }
            cur_pos = cur.next;
        }

        return result;
    }

    // Find exact key-value pair
    bool findExact(const Key &key, int val) {
        if (head == 0) return false;
        int cur_pos = head;
        BlockNode cur;
        while (cur_pos != 0) {
            fm.read(cur_pos, cur);
            if (cur.count > 0 && cur.keys[cur.count-1] >= key) {
                for (int i = 0; i < cur.count; i++) {
                    if (cur.keys[i] == key && cur.values[i] == val) return true;
                    if (cur.keys[i] > key) return false;
                }
            }
            cur_pos = cur.next;
        }
        return false;
    }

    // Get all entries (for show all books)
    std::vector<int> getAll() {
        std::vector<int> result;
        if (head == 0) return result;
        int cur_pos = head;
        BlockNode cur;
        while (cur_pos != 0) {
            fm.read(cur_pos, cur);
            for (int i = 0; i < cur.count; i++) {
                result.push_back(cur.values[i]);
            }
            cur_pos = cur.next;
        }
        return result;
    }
};

// ===================== Data Structures =====================

struct FixStr30 {
    char data[31];
    FixStr30() { memset(data, 0, 31); }
    FixStr30(const std::string &s) { memset(data, 0, 31); strncpy(data, s.c_str(), 30); }
    std::string str() const { return std::string(data); }
    bool operator==(const FixStr30 &o) const { return strcmp(data, o.data) == 0; }
};

struct FixStr60 {
    char data[61];
    FixStr60() { memset(data, 0, 61); }
    FixStr60(const std::string &s) { memset(data, 0, 61); strncpy(data, s.c_str(), 60); }
    std::string str() const { return std::string(data); }
};

struct FixStr20 {
    char data[21];
    FixStr20() { memset(data, 0, 21); }
    FixStr20(const std::string &s) { memset(data, 0, 21); strncpy(data, s.c_str(), 20); }
    std::string str() const { return std::string(data); }
    bool operator<(const FixStr20 &o) const { return strcmp(data, o.data) < 0; }
};

struct Account {
    char userId[31];
    char password[31];
    int privilege;
    char username[31];

    Account() {
        memset(userId, 0, 31);
        memset(password, 0, 31);
        privilege = 0;
        memset(username, 0, 31);
    }
};

struct Book {
    char isbn[21];
    char name[61];
    char author[61];
    char keyword[61];
    double price;
    int stock;

    Book() {
        memset(isbn, 0, 21);
        memset(name, 0, 61);
        memset(author, 0, 61);
        memset(keyword, 0, 61);
        price = 0;
        stock = 0;
    }
};

struct FinanceRecord {
    double income;   // positive for buy
    double expense;  // positive for import
};

// ===================== Global Storage =====================

FileManager accountFile;
FileManager bookFile;
FileManager financeFile;
FileManager logFile;

BlockList accountIndex;   // userId -> file pos in accountFile
BlockList isbnIndex;      // isbn -> file pos in bookFile
BlockList nameIndex;      // name -> file pos in bookFile
BlockList authorIndex;    // author -> file pos in bookFile
BlockList keywordIndex;   // keyword -> file pos in bookFile

// Finance stored sequentially
int financeCount = 0;
int financeCountPos = 0;

// Log stored sequentially
int logCount = 0;
int logCountPos = 0;

struct LogEntry {
    char who[31];
    char action[201];
};

// ===================== Login Stack =====================

struct LoginInfo {
    int accountPos; // position in accountFile
    int privilege;
    int selectedBookPos; // -1 if none
    char userId[31];
};

std::vector<LoginInfo> loginStack;

int currentPrivilege() {
    if (loginStack.empty()) return 0;
    return loginStack.back().privilege;
}

std::string currentUserId() {
    if (loginStack.empty()) return "";
    return std::string(loginStack.back().userId);
}

int currentSelectedBook() {
    if (loginStack.empty()) return -1;
    return loginStack.back().selectedBookPos;
}

void setSelectedBook(int pos) {
    if (!loginStack.empty()) {
        loginStack.back().selectedBookPos = pos;
    }
}

// ===================== Initialization =====================

void initStorage() {
    accountFile.init("account_data");
    bookFile.init("book_data");
    financeFile.init("finance_data");
    logFile.init("log_data");

    accountIndex.init("account_idx");
    isbnIndex.init("isbn_idx");
    nameIndex.init("name_idx");
    authorIndex.init("author_idx");
    keywordIndex.init("keyword_idx");

    // Check if root exists
    auto roots = accountIndex.find("root");
    if (roots.empty()) {
        // Create root account
        Account root;
        strncpy(root.userId, "root", 30);
        strncpy(root.password, "sjtu", 30);
        root.privilege = 7;
        strncpy(root.username, "root", 30);
        int pos = accountFile.alloc(sizeof(Account));
        accountFile.write(pos, root);
        accountIndex.insert(Key("root", "root"), pos);
    }

    // Initialize finance count
    int fNext;
    financeFile.read(0, fNext);
    if (fNext == (int)sizeof(int)) {
        // New file
        financeCountPos = financeFile.alloc(sizeof(int));
        financeCount = 0;
        financeFile.write(financeCountPos, financeCount);
    } else {
        financeCountPos = sizeof(int);
        financeFile.read(financeCountPos, financeCount);
    }

    // Initialize log count
    int lNext;
    logFile.read(0, lNext);
    if (lNext == (int)sizeof(int)) {
        logCountPos = logFile.alloc(sizeof(int));
        logCount = 0;
        logFile.write(logCountPos, logCount);
    } else {
        logCountPos = sizeof(int);
        logFile.read(logCountPos, logCount);
    }
}

// ===================== Helper Functions =====================

void addFinanceRecord(double income, double expense) {
    FinanceRecord rec;
    rec.income = income;
    rec.expense = expense;
    int pos = financeFile.alloc(sizeof(FinanceRecord));
    financeFile.write(pos, rec);
    financeCount++;
    financeFile.write(financeCountPos, financeCount);
}

void addLogEntry(const std::string &who, const std::string &action) {
    LogEntry entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.who, who.c_str(), 30);
    strncpy(entry.action, action.c_str(), 200);
    int pos = logFile.alloc(sizeof(LogEntry));
    logFile.write(pos, entry);
    logCount++;
    logFile.write(logCountPos, logCount);
}

bool isValidUserId(const std::string &s) {
    if (s.empty() || s.length() > 30) return false;
    for (char c : s) {
        if (!isalnum(c) && c != '_') return false;
    }
    return true;
}

bool isValidPassword(const std::string &s) {
    return isValidUserId(s); // same rules
}

bool isValidUsername(const std::string &s) {
    if (s.empty() || s.length() > 30) return false;
    for (char c : s) {
        if (c < 33 || c > 126) return false; // visible ASCII only
    }
    return true;
}

bool isValidISBN(const std::string &s) {
    if (s.empty() || s.length() > 20) return false;
    for (char c : s) {
        if (c < 33 || c > 126) return false;
    }
    return true;
}

bool isValidBookName(const std::string &s) {
    if (s.empty() || s.length() > 60) return false;
    for (char c : s) {
        if (c < 33 || c > 126) return false;
        if (c == '"') return false;
    }
    return true;
}

bool isValidAuthor(const std::string &s) {
    return isValidBookName(s); // same rules
}

bool isValidKeyword(const std::string &s) {
    if (s.empty() || s.length() > 60) return false;
    for (char c : s) {
        if (c < 33 || c > 126) return false;
        if (c == '"') return false;
    }
    // Check no empty segments and no duplicates
    // Split by |
    std::vector<std::string> segs;
    std::string cur;
    for (char c : s) {
        if (c == '|') {
            if (cur.empty()) return false;
            segs.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (cur.empty()) return false;
    segs.push_back(cur);
    return true;
}

bool isValidKeywordForModify(const std::string &s) {
    if (!isValidKeyword(s)) return false;
    // Check for duplicates
    std::vector<std::string> segs;
    std::string cur;
    for (char c : s) {
        if (c == '|') {
            segs.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    segs.push_back(cur);
    std::vector<std::string> sorted_segs = segs;
    std::sort(sorted_segs.begin(), sorted_segs.end());
    for (int i = 1; i < (int)sorted_segs.size(); i++) {
        if (sorted_segs[i] == sorted_segs[i-1]) return false;
    }
    return true;
}

bool isValidKeywordSingle(const std::string &s) {
    // For show -keyword="...", must be single keyword (no |)
    if (!isValidKeyword(s)) return false;
    for (char c : s) {
        if (c == '|') return false;
    }
    return true;
}

bool isDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!isdigit(c)) return false;
    }
    return true;
}

bool isValidQuantity(const std::string &s) {
    if (!isDigits(s) || s.length() > 10) return false;
    long long val = std::stoll(s);
    return val >= 0 && val <= 2147483647LL;
}

bool isPositiveQuantity(const std::string &s) {
    if (!isValidQuantity(s)) return false;
    long long val = std::stoll(s);
    return val > 0;
}

bool isValidPrice(const std::string &s) {
    if (s.empty() || s.length() > 13) return false;
    int dot_count = 0;
    int dot_pos = -1;
    for (int i = 0; i < (int)s.length(); i++) {
        if (s[i] == '.') {
            dot_count++;
            if (dot_count > 1) return false;
            dot_pos = i;
        } else if (!isdigit(s[i])) {
            return false;
        }
    }
    if (dot_pos == 0) return false; // no digits before dot
    if (dot_pos != -1 && dot_pos == (int)s.length() - 1) return false; // trailing dot
    // Must have at least one digit (not just dots)
    if (dot_count == (int)s.length()) return false;
    return true;
}

bool isPositivePrice(const std::string &s) {
    if (!isValidPrice(s)) return false;
    double val = std::stod(s);
    return val > 0;
}

bool isValidPrivilege(const std::string &s) {
    if (s.length() != 1) return false;
    return s == "1" || s == "3" || s == "7";
}

bool isValidCount(const std::string &s) {
    if (!isDigits(s) || s.length() > 10) return false;
    long long val = std::stoll(s);
    return val >= 0 && val <= 2147483647LL;
}

// Find account by userId, return file position or -1
int findAccount(const std::string &userId) {
    auto results = accountIndex.find(userId);
    if (results.empty()) return -1;
    return results[0];
}

// Find book by ISBN, return file position or -1
int findBook(const std::string &isbn) {
    auto results = isbnIndex.find(isbn);
    if (results.empty()) return -1;
    return results[0];
}

// Check if userId is logged in (anywhere in stack)
bool isLoggedIn(const std::string &userId) {
    for (auto &info : loginStack) {
        if (strcmp(info.userId, userId.c_str()) == 0) return true;
    }
    return false;
}

// Split keywords by |
std::vector<std::string> splitKeywords(const std::string &kw) {
    std::vector<std::string> result;
    std::string cur;
    for (char c : kw) {
        if (c == '|') {
            if (!cur.empty()) result.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) result.push_back(cur);
    return result;
}

// ===================== Command Tokenizer =====================

std::vector<std::string> tokenize(const std::string &line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// ===================== Command Handlers =====================

void cmdSu(const std::vector<std::string> &args) {
    // su [UserID] ([Password])?
    if (args.size() < 2 || args.size() > 3) { std::cout << "Invalid\n"; return; }

    std::string userId = args[1];
    if (!isValidUserId(userId)) { std::cout << "Invalid\n"; return; }

    int pos = findAccount(userId);
    if (pos == -1) { std::cout << "Invalid\n"; return; }

    Account acc;
    accountFile.read(pos, acc);

    if (args.size() == 3) {
        std::string password = args[2];
        if (!isValidPassword(password)) { std::cout << "Invalid\n"; return; }
        if (strcmp(acc.password, password.c_str()) != 0) { std::cout << "Invalid\n"; return; }
    } else {
        // Password omitted - current privilege must be higher
        if (currentPrivilege() <= acc.privilege) { std::cout << "Invalid\n"; return; }
    }

    LoginInfo info;
    info.accountPos = pos;
    info.privilege = acc.privilege;
    info.selectedBookPos = -1;
    memset(info.userId, 0, 31);
    strncpy(info.userId, acc.userId, 30);
    loginStack.push_back(info);

    addLogEntry(currentUserId(), "su " + userId);
}

void cmdLogout(const std::vector<std::string> &args) {
    if (args.size() != 1) { std::cout << "Invalid\n"; return; }
    if (loginStack.empty()) { std::cout << "Invalid\n"; return; }
    std::string who = currentUserId();
    loginStack.pop_back();
    addLogEntry(who, "logout");
}

void cmdRegister(const std::vector<std::string> &args) {
    // register [UserID] [Password] [Username]
    if (args.size() != 4) { std::cout << "Invalid\n"; return; }

    std::string userId = args[1];
    std::string password = args[2];
    std::string username = args[3];

    if (!isValidUserId(userId) || !isValidPassword(password) || !isValidUsername(username)) {
        std::cout << "Invalid\n"; return;
    }

    if (findAccount(userId) != -1) { std::cout << "Invalid\n"; return; }

    Account acc;
    strncpy(acc.userId, userId.c_str(), 30);
    strncpy(acc.password, password.c_str(), 30);
    acc.privilege = 1;
    strncpy(acc.username, username.c_str(), 30);

    int pos = accountFile.alloc(sizeof(Account));
    accountFile.write(pos, acc);
    accountIndex.insert(Key(userId, userId), pos);

    addLogEntry(currentUserId().empty() ? "(guest)" : currentUserId(), "register " + userId);
}

void cmdPasswd(const std::vector<std::string> &args) {
    // passwd [UserID] ([CurrentPassword])? [NewPassword]
    if (args.size() < 3 || args.size() > 4) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 1) { std::cout << "Invalid\n"; return; }

    std::string userId = args[1];
    if (!isValidUserId(userId)) { std::cout << "Invalid\n"; return; }

    int pos = findAccount(userId);
    if (pos == -1) { std::cout << "Invalid\n"; return; }

    Account acc;
    accountFile.read(pos, acc);

    if (args.size() == 4) {
        std::string curPwd = args[2];
        std::string newPwd = args[3];
        if (!isValidPassword(curPwd) || !isValidPassword(newPwd)) { std::cout << "Invalid\n"; return; }
        if (strcmp(acc.password, curPwd.c_str()) != 0) { std::cout << "Invalid\n"; return; }
        strncpy(acc.password, newPwd.c_str(), 30);
    } else {
        // 3 args: passwd userId newPassword
        // Current password can be omitted only if privilege is {7}
        if (currentPrivilege() != 7) { std::cout << "Invalid\n"; return; }
        std::string newPwd = args[2];
        if (!isValidPassword(newPwd)) { std::cout << "Invalid\n"; return; }
        strncpy(acc.password, newPwd.c_str(), 30);
    }

    accountFile.write(pos, acc);
    addLogEntry(currentUserId(), "passwd " + userId);
}

void cmdUseradd(const std::vector<std::string> &args) {
    // useradd [UserID] [Password] [Privilege] [Username]
    if (args.size() != 5) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 3) { std::cout << "Invalid\n"; return; }

    std::string userId = args[1];
    std::string password = args[2];
    std::string privStr = args[3];
    std::string username = args[4];

    if (!isValidUserId(userId) || !isValidPassword(password) || !isValidPrivilege(privStr) || !isValidUsername(username)) {
        std::cout << "Invalid\n"; return;
    }

    int priv = std::stoi(privStr);
    if (priv >= currentPrivilege()) { std::cout << "Invalid\n"; return; }

    if (findAccount(userId) != -1) { std::cout << "Invalid\n"; return; }

    Account acc;
    strncpy(acc.userId, userId.c_str(), 30);
    strncpy(acc.password, password.c_str(), 30);
    acc.privilege = priv;
    strncpy(acc.username, username.c_str(), 30);

    int pos = accountFile.alloc(sizeof(Account));
    accountFile.write(pos, acc);
    accountIndex.insert(Key(userId, userId), pos);

    addLogEntry(currentUserId(), "useradd " + userId);
}

void cmdDelete(const std::vector<std::string> &args) {
    // delete [UserID]
    if (args.size() != 2) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 7) { std::cout << "Invalid\n"; return; }

    std::string userId = args[1];
    if (!isValidUserId(userId)) { std::cout << "Invalid\n"; return; }

    int pos = findAccount(userId);
    if (pos == -1) { std::cout << "Invalid\n"; return; }

    if (isLoggedIn(userId)) { std::cout << "Invalid\n"; return; }

    // Remove from index (we don't reclaim file space, just remove from index)
    accountIndex.erase(Key(userId, userId), pos);

    addLogEntry(currentUserId(), "delete " + userId);
}

void cmdShow(const std::vector<std::string> &args) {
    if (currentPrivilege() < 1) { std::cout << "Invalid\n"; return; }

    if (args.size() == 1) {
        // Show all books sorted by ISBN
        auto all = isbnIndex.getAll();
        if (all.empty()) {
            std::cout << "\n";
            return;
        }
        // They're already sorted by ISBN since isbnIndex is sorted
        for (int pos : all) {
            Book book;
            bookFile.read(pos, book);
            std::cout << book.isbn << "\t" << book.name << "\t" << book.author << "\t"
                      << book.keyword << "\t" << std::fixed << std::setprecision(2) << book.price
                      << "\t" << book.stock << "\n";
        }
        return;
    }

    if (args.size() != 2) { std::cout << "Invalid\n"; return; }

    std::string param = args[1];

    if (param.substr(0, 6) == "-ISBN=") {
        std::string isbn = param.substr(6);
        if (!isValidISBN(isbn)) { std::cout << "Invalid\n"; return; }
        int pos = findBook(isbn);
        if (pos == -1) {
            std::cout << "\n";
            return;
        }
        Book book;
        bookFile.read(pos, book);
        std::cout << book.isbn << "\t" << book.name << "\t" << book.author << "\t"
                  << book.keyword << "\t" << std::fixed << std::setprecision(2) << book.price
                  << "\t" << book.stock << "\n";
    } else if (param.substr(0, 7) == "-name=\"" && param.back() == '"') {
        std::string name = param.substr(7, param.length() - 8);
        if (!isValidBookName(name)) { std::cout << "Invalid\n"; return; }
        auto results = nameIndex.find(name);
        if (results.empty()) {
            std::cout << "\n";
            return;
        }
        // Need to sort by ISBN
        std::vector<Book> books;
        for (int pos : results) {
            Book book;
            bookFile.read(pos, book);
            books.push_back(book);
        }
        std::sort(books.begin(), books.end(), [](const Book &a, const Book &b) {
            return strcmp(a.isbn, b.isbn) < 0;
        });
        for (auto &book : books) {
            std::cout << book.isbn << "\t" << book.name << "\t" << book.author << "\t"
                      << book.keyword << "\t" << std::fixed << std::setprecision(2) << book.price
                      << "\t" << book.stock << "\n";
        }
    } else if (param.substr(0, 9) == "-author=\"" && param.back() == '"') {
        std::string author = param.substr(9, param.length() - 10);
        if (!isValidAuthor(author)) { std::cout << "Invalid\n"; return; }
        auto results = authorIndex.find(author);
        if (results.empty()) {
            std::cout << "\n";
            return;
        }
        std::vector<Book> books;
        for (int pos : results) {
            Book book;
            bookFile.read(pos, book);
            books.push_back(book);
        }
        std::sort(books.begin(), books.end(), [](const Book &a, const Book &b) {
            return strcmp(a.isbn, b.isbn) < 0;
        });
        for (auto &book : books) {
            std::cout << book.isbn << "\t" << book.name << "\t" << book.author << "\t"
                      << book.keyword << "\t" << std::fixed << std::setprecision(2) << book.price
                      << "\t" << book.stock << "\n";
        }
    } else if (param.substr(0, 10) == "-keyword=\"" && param.back() == '"') {
        std::string keyword = param.substr(10, param.length() - 11);
        if (!isValidKeywordSingle(keyword)) { std::cout << "Invalid\n"; return; }
        auto results = keywordIndex.find(keyword);
        if (results.empty()) {
            std::cout << "\n";
            return;
        }
        std::vector<Book> books;
        for (int pos : results) {
            Book book;
            bookFile.read(pos, book);
            books.push_back(book);
        }
        std::sort(books.begin(), books.end(), [](const Book &a, const Book &b) {
            return strcmp(a.isbn, b.isbn) < 0;
        });
        for (auto &book : books) {
            std::cout << book.isbn << "\t" << book.name << "\t" << book.author << "\t"
                      << book.keyword << "\t" << std::fixed << std::setprecision(2) << book.price
                      << "\t" << book.stock << "\n";
        }
    } else {
        std::cout << "Invalid\n";
    }
}

void cmdBuy(const std::vector<std::string> &args) {
    // buy [ISBN] [Quantity]
    if (args.size() != 3) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 1) { std::cout << "Invalid\n"; return; }

    std::string isbn = args[1];
    std::string qtyStr = args[2];

    if (!isValidISBN(isbn)) { std::cout << "Invalid\n"; return; }
    if (!isPositiveQuantity(qtyStr)) { std::cout << "Invalid\n"; return; }

    int pos = findBook(isbn);
    if (pos == -1) { std::cout << "Invalid\n"; return; }

    long long qty = std::stoll(qtyStr);

    Book book;
    bookFile.read(pos, book);

    if (book.stock < qty) { std::cout << "Invalid\n"; return; }

    double total = book.price * qty;
    book.stock -= (int)qty;
    bookFile.write(pos, book);

    std::cout << std::fixed << std::setprecision(2) << total << "\n";

    addFinanceRecord(total, 0);
    addLogEntry(currentUserId(), "buy " + isbn + " " + qtyStr);
}

void cmdSelect(const std::vector<std::string> &args) {
    // select [ISBN]
    if (args.size() != 2) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 3) { std::cout << "Invalid\n"; return; }

    std::string isbn = args[1];
    if (!isValidISBN(isbn)) { std::cout << "Invalid\n"; return; }

    int pos = findBook(isbn);
    if (pos == -1) {
        // Create new book
        Book book;
        strncpy(book.isbn, isbn.c_str(), 20);
        pos = bookFile.alloc(sizeof(Book));
        bookFile.write(pos, book);
        isbnIndex.insert(Key(isbn, isbn), pos);
    }

    setSelectedBook(pos);
    addLogEntry(currentUserId(), "select " + isbn);
}

void cmdModify(const std::vector<std::string> &args) {
    // modify (-ISBN=[ISBN] | -name="[BookName]" | -author="[Author]" | -keyword="[Keyword]" | -price=[Price])+
    if (args.size() < 2) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 3) { std::cout << "Invalid\n"; return; }

    int bookPos = currentSelectedBook();
    if (bookPos == -1) { std::cout << "Invalid\n"; return; }

    Book book;
    bookFile.read(bookPos, book);

    std::string newIsbn, newName, newAuthor, newKeyword;
    double newPrice = -1;
    bool hasIsbn = false, hasName = false, hasAuthor = false, hasKeyword = false, hasPrice = false;

    for (int i = 1; i < (int)args.size(); i++) {
        const std::string &param = args[i];

        if (param.substr(0, 6) == "-ISBN=") {
            if (hasIsbn) { std::cout << "Invalid\n"; return; } // duplicate
            hasIsbn = true;
            newIsbn = param.substr(6);
            if (!isValidISBN(newIsbn)) { std::cout << "Invalid\n"; return; }
        } else if (param.substr(0, 7) == "-name=\"" && param.back() == '"') {
            if (hasName) { std::cout << "Invalid\n"; return; }
            hasName = true;
            newName = param.substr(7, param.length() - 8);
            if (!isValidBookName(newName)) { std::cout << "Invalid\n"; return; }
        } else if (param.substr(0, 9) == "-author=\"" && param.back() == '"') {
            if (hasAuthor) { std::cout << "Invalid\n"; return; }
            hasAuthor = true;
            newAuthor = param.substr(9, param.length() - 10);
            if (!isValidAuthor(newAuthor)) { std::cout << "Invalid\n"; return; }
        } else if (param.substr(0, 10) == "-keyword=\"" && param.back() == '"') {
            if (hasKeyword) { std::cout << "Invalid\n"; return; }
            hasKeyword = true;
            newKeyword = param.substr(10, param.length() - 11);
            if (!isValidKeywordForModify(newKeyword)) { std::cout << "Invalid\n"; return; }
        } else if (param.substr(0, 7) == "-price=") {
            if (hasPrice) { std::cout << "Invalid\n"; return; }
            hasPrice = true;
            std::string priceStr = param.substr(7);
            if (!isValidPrice(priceStr)) { std::cout << "Invalid\n"; return; }
            newPrice = std::stod(priceStr);
        } else {
            std::cout << "Invalid\n"; return;
        }
    }

    // Check ISBN constraints
    if (hasIsbn) {
        if (newIsbn == std::string(book.isbn)) { std::cout << "Invalid\n"; return; } // same ISBN
        if (findBook(newIsbn) != -1) { std::cout << "Invalid\n"; return; } // ISBN already exists
    }

    // Apply changes
    // Remove old indices
    std::string oldIsbn = book.isbn;
    std::string oldName = book.name;
    std::string oldAuthor = book.author;
    std::string oldKeyword = book.keyword;

    if (hasIsbn) {
        isbnIndex.erase(Key(oldIsbn, oldIsbn), bookPos);
        memset(book.isbn, 0, 21);
        strncpy(book.isbn, newIsbn.c_str(), 20);
        isbnIndex.insert(Key(newIsbn, newIsbn), bookPos);
    }

    if (hasName) {
        if (!oldName.empty()) nameIndex.erase(Key(oldName, oldIsbn), bookPos);
        memset(book.name, 0, 61);
        strncpy(book.name, newName.c_str(), 60);
        std::string curIsbn = hasIsbn ? newIsbn : oldIsbn;
        nameIndex.insert(Key(newName, curIsbn), bookPos);
    } else if (hasIsbn && !oldName.empty()) {
        // ISBN changed, update name index key
        nameIndex.erase(Key(oldName, oldIsbn), bookPos);
        nameIndex.insert(Key(oldName, newIsbn), bookPos);
    }

    if (hasAuthor) {
        if (!oldAuthor.empty()) authorIndex.erase(Key(oldAuthor, oldIsbn), bookPos);
        memset(book.author, 0, 61);
        strncpy(book.author, newAuthor.c_str(), 60);
        std::string curIsbn = hasIsbn ? newIsbn : oldIsbn;
        authorIndex.insert(Key(newAuthor, curIsbn), bookPos);
    } else if (hasIsbn && !oldAuthor.empty()) {
        authorIndex.erase(Key(oldAuthor, oldIsbn), bookPos);
        authorIndex.insert(Key(oldAuthor, newIsbn), bookPos);
    }

    if (hasKeyword) {
        // Remove old keywords from index
        auto oldKws = splitKeywords(oldKeyword);
        for (auto &kw : oldKws) {
            keywordIndex.erase(Key(kw, oldIsbn), bookPos);
        }
        memset(book.keyword, 0, 61);
        strncpy(book.keyword, newKeyword.c_str(), 60);
        std::string curIsbn = hasIsbn ? newIsbn : oldIsbn;
        auto newKws = splitKeywords(newKeyword);
        for (auto &kw : newKws) {
            keywordIndex.insert(Key(kw, curIsbn), bookPos);
        }
    } else if (hasIsbn && !oldKeyword.empty()) {
        auto kws = splitKeywords(oldKeyword);
        for (auto &kw : kws) {
            keywordIndex.erase(Key(kw, oldIsbn), bookPos);
            keywordIndex.insert(Key(kw, newIsbn), bookPos);
        }
    }

    if (hasPrice) {
        book.price = newPrice;
    }

    bookFile.write(bookPos, book);
    addLogEntry(currentUserId(), "modify book");
}

void cmdImport(const std::vector<std::string> &args) {
    // import [Quantity] [TotalCost]
    if (args.size() != 3) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 3) { std::cout << "Invalid\n"; return; }

    int bookPos = currentSelectedBook();
    if (bookPos == -1) { std::cout << "Invalid\n"; return; }

    std::string qtyStr = args[1];
    std::string costStr = args[2];

    if (!isPositiveQuantity(qtyStr)) { std::cout << "Invalid\n"; return; }
    if (!isPositivePrice(costStr)) { std::cout << "Invalid\n"; return; }

    long long qty = std::stoll(qtyStr);
    double cost = std::stod(costStr);

    Book book;
    bookFile.read(bookPos, book);
    book.stock += (int)qty;
    bookFile.write(bookPos, book);

    addFinanceRecord(0, cost);
    addLogEntry(currentUserId(), "import " + qtyStr + " " + costStr);
}

void cmdShowFinance(const std::vector<std::string> &args) {
    // show finance ([Count])?
    if (currentPrivilege() < 7) { std::cout << "Invalid\n"; return; }

    if (args.size() == 2) {
        // show finance - all transactions
        double totalIncome = 0, totalExpense = 0;
        // Read all finance records
        // Records start after financeCountPos + sizeof(int)
        int startPos = financeCountPos + sizeof(int);
        for (int i = 0; i < financeCount; i++) {
            FinanceRecord rec;
            financeFile.read(startPos + i * (int)sizeof(FinanceRecord), rec);
            totalIncome += rec.income;
            totalExpense += rec.expense;
        }
        std::cout << "+ " << std::fixed << std::setprecision(2) << totalIncome
                  << " - " << std::fixed << std::setprecision(2) << totalExpense << "\n";
    } else if (args.size() == 3) {
        std::string countStr = args[2];
        if (!isValidCount(countStr)) { std::cout << "Invalid\n"; return; }
        long long count = std::stoll(countStr);
        if (count == 0) {
            std::cout << "\n";
            return;
        }
        if (count > financeCount) { std::cout << "Invalid\n"; return; }

        double totalIncome = 0, totalExpense = 0;
        int startPos = financeCountPos + sizeof(int);
        int startIdx = financeCount - (int)count;
        for (int i = startIdx; i < financeCount; i++) {
            FinanceRecord rec;
            financeFile.read(startPos + i * (int)sizeof(FinanceRecord), rec);
            totalIncome += rec.income;
            totalExpense += rec.expense;
        }
        std::cout << "+ " << std::fixed << std::setprecision(2) << totalIncome
                  << " - " << std::fixed << std::setprecision(2) << totalExpense << "\n";
    } else {
        std::cout << "Invalid\n";
    }
}

void cmdLog(const std::vector<std::string> &args) {
    if (args.size() != 1) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 7) { std::cout << "Invalid\n"; return; }

    int startPos = logCountPos + sizeof(int);
    for (int i = 0; i < logCount; i++) {
        LogEntry entry;
        logFile.read(startPos + i * (int)sizeof(LogEntry), entry);
        std::cout << entry.who << ": " << entry.action << "\n";
    }
    if (logCount == 0) std::cout << "\n";
}

void cmdReportFinance(const std::vector<std::string> &args) {
    if (args.size() != 2) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 7) { std::cout << "Invalid\n"; return; }

    // Self-defined format
    std::cout << "--- Finance Report ---\n";
    double totalIncome = 0, totalExpense = 0;
    int startPos = financeCountPos + sizeof(int);
    for (int i = 0; i < financeCount; i++) {
        FinanceRecord rec;
        financeFile.read(startPos + i * (int)sizeof(FinanceRecord), rec);
        totalIncome += rec.income;
        totalExpense += rec.expense;
        if (rec.income > 0) {
            std::cout << "Transaction " << (i+1) << ": Income +" << std::fixed << std::setprecision(2) << rec.income << "\n";
        } else {
            std::cout << "Transaction " << (i+1) << ": Expense -" << std::fixed << std::setprecision(2) << rec.expense << "\n";
        }
    }
    std::cout << "Total: +" << std::fixed << std::setprecision(2) << totalIncome
              << " -" << std::fixed << std::setprecision(2) << totalExpense << "\n";
    std::cout << "--- End of Finance Report ---\n";
}

void cmdReportEmployee(const std::vector<std::string> &args) {
    if (args.size() != 2) { std::cout << "Invalid\n"; return; }
    if (currentPrivilege() < 7) { std::cout << "Invalid\n"; return; }

    std::cout << "--- Employee Report ---\n";
    int startPos = logCountPos + sizeof(int);
    for (int i = 0; i < logCount; i++) {
        LogEntry entry;
        logFile.read(startPos + i * (int)sizeof(LogEntry), entry);
        std::cout << entry.who << ": " << entry.action << "\n";
    }
    std::cout << "--- End of Employee Report ---\n";
}

// ===================== Main =====================

int main() {
    initStorage();

    std::string line;
    while (std::getline(std::cin, line)) {
        // Strip trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto args = tokenize(line);
        if (args.empty()) continue; // blank line

        const std::string &cmd = args[0];

        if (cmd == "quit" || cmd == "exit") {
            if (args.size() != 1) { std::cout << "Invalid\n"; continue; }
            break;
        } else if (cmd == "su") {
            cmdSu(args);
        } else if (cmd == "logout") {
            cmdLogout(args);
        } else if (cmd == "register") {
            cmdRegister(args);
        } else if (cmd == "passwd") {
            cmdPasswd(args);
        } else if (cmd == "useradd") {
            cmdUseradd(args);
        } else if (cmd == "delete") {
            cmdDelete(args);
        } else if (cmd == "show") {
            if (args.size() >= 2 && args[1] == "finance") {
                cmdShowFinance(args);
            } else {
                cmdShow(args);
            }
        } else if (cmd == "buy") {
            cmdBuy(args);
        } else if (cmd == "select") {
            cmdSelect(args);
        } else if (cmd == "modify") {
            cmdModify(args);
        } else if (cmd == "import") {
            cmdImport(args);
        } else if (cmd == "log") {
            cmdLog(args);
        } else if (cmd == "report") {
            if (args.size() >= 2 && args[1] == "finance") {
                cmdReportFinance(args);
            } else if (args.size() >= 2 && args[1] == "employee") {
                cmdReportEmployee(args);
            } else {
                std::cout << "Invalid\n";
            }
        } else {
            std::cout << "Invalid\n";
        }
    }

    return 0;
}
