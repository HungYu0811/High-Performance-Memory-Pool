#include <iostream>
#include <cstddef> // 為了使用 size_t
using namespace std;
// 1. 定義豆腐塊的骨架（車廂）
// 當這塊記憶體沒人用的時候，它裡面要塞一個指標，用來指向下一塊空閒豆腐
struct Node {
    Node* next;
};
struct Player{
    int id;
    int hp;
    int mp;
    string name;
};
class SimpleMemoryPool {
private:
    char* rawMemory;     // 儲存整塊大記憶體的起點
    Node* freeListHead;  // 空閒鏈表的車頭，永遠指向當前「最新鮮、可用」的豆腐

public:
    // 建構子：跟系統挖地，然後當場切豆腐
    SimpleMemoryPool() {
        // 總共挖 96 字節（可以切出 3 塊 32 字節的豆腐）
        rawMemory = new char[96]; 

        // 💥 開始切豆腐！利用指標算術（Pointer Arithmetic）算出每塊豆腐的記憶體地址
        Node* block1 = reinterpret_cast<Node*>(rawMemory);       // 第 1 塊：位元組 0
        Node* block2 = reinterpret_cast<Node*>(rawMemory + 32);  // 第 2 塊：位元組 32
        Node* block3 = reinterpret_cast<Node*>(rawMemory + 64);  // 第 3 塊：位元組 64

        // 💥 讓豆腐手牽手，串成鏈表！這就是你這週在 LeetCode 練的鏈表串接
        block1->next = block2;
        block2->next = block3;
        block3->next = nullptr; // 最後一塊豆腐後面沒人了

        // 車頭停在第一塊豆腐
        freeListHead = block1;
        cout << "[init] cut 96 untit tofu cut in 3 pieces   ，head of the link list is at " << freeListHead << "\n\n";
    }

    // 2. 借出豆腐 (Allocate) -> 等同於 LeetCode 的「移出頭節點」
    void* allocate() {
        if (freeListHead == nullptr) {
            cout << "[Error] Run out of memory ！Cannot alllocate.\n";
            return nullptr;
        }

        // 把目前的車頭拿出來，準備借給客人
        Node* chunkToBorrow = freeListHead;

        // 💥 快慢指標的位移！車頭退一步，指向下一塊空閒豆腐
        freeListHead = freeListHead->next;

        cout << " -> lend one peace of tofu, address : " << chunkToBorrow 
                  << " | Next tofu is at : " << freeListHead << "\n";
        
        return reinterpret_cast<void*>(chunkToBorrow);
    }

    // 3. 還回豆腐 (Deallocate) -> 等同於 LeetCode 的「把新節點插到開頭」
    void deallocate(void* address) {
        if (address == nullptr) return;

        // 把這塊被歸還的記憶體，重新看成一個 Node
        Node* returnedChunk = reinterpret_cast<Node*>(address);

        // 💥 把這節車廂插回目前的鏈表開頭
        returnedChunk->next = freeListHead;

        // 讓車頭重新停在剛剛歸還的這個節點上
        freeListHead = returnedChunk;

        cout << " <- Customer return the tofu, address : " << address 
                  << " | Now new head of the list is : " << freeListHead << "\n";
    }

    // 解構子：要把跟系統借的大地還給人家，不然會 Memory Leak
    ~SimpleMemoryPool() {
        delete[] rawMemory;
        cout << "\n[Finish] Memory take back success, finish！\n";
    }
};

int main() {
    // 啟動我們的實驗記憶體池
    SimpleMemoryPool pool;
    cout << "start to borrow memroy and put player info in memory" << endl;
    void* memory1 = pool.allocate();
    Player* player1 = new (memory1) Player{19,200,400,"vicky"};
    void* memory2 = pool.allocate();
    Player* player2 = new (memory2) Player{48,500,300,"Leo"};
    cout << "Check player info in memory correctly" <<endl;
    cout << "player1 name:" << player1 -> name << "| HP:" << player1 -> hp << "| MP:" << player1 -> mp << "|id:" << player1 -> id << "| address:" << player1 << endl;
    cout << "player2 name:" << player2 -> name << "| HP:" << player2 -> hp << "| MP:" << player2 -> mp << "|id:" << player2 -> id << "| address:" << player2 << endl;
    cout << "Test return address from plater2" << endl; 
    player2->~Player();
    pool.deallocate(memory2);
    cout << "put new player in the return address" << endl;
    void* memory3 = pool.allocate();
    Player* player3 = new (memory3) Player{4819,700,400,"Trout"};
    cout << "player3 name:" << player3 -> name << "| HP:" << player3 -> hp << "| MP:" << player3 -> mp << "|id:" << player3 -> id << "| address:" << player3 << endl;
    cout << "check if trout address is same as Leo" << endl;
    cout << "end the code" << endl;
    
    return 0;
}
