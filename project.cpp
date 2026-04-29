#include <thread>        // for multithreading
#include <mutex>         // for locks
#include <condition_variable> // for signaling
#include <queue>         // shared queue
#include <semaphore>     // (C++20) chef control
#include <atomic>    // safe counters
#include<chrono>
#include<cstdlib>
#include<iostream>
using namespace std;
struct Order { 
    int id;
    int priority;//1= vip or 0 = normal
    Order(int i, int p) : id(i),priority(p){}
};

struct Compare{
    bool operator()(Order a , Order b){
    if (a.priority == 0 && b.priority == 1) {
        return true; //b is VIP, so b comes first
     } else {
     return false;
     }
  }
};    

//SHARED RESOURCES BY BOTH THREADS I.E: producer and consumer
priority_queue<Order, vector<Order>, Compare> orderQueue;
mutex mtx;
condition_variable cv;
atomic<int> orderCounter(0);
bool finished = false;
counting_semaphore<3> kitchenSlots(3); //max 3 chefs working : simulates limited CPU resources
atomic<int> completedOrders(0);
atomic<int> activeChefs(0);
void waiter(int id) {
    for (int i = 0; i < 5; i++) {

        this_thread::sleep_for(chrono::milliseconds(rand() % 500));
        int orderId = ++orderCounter;
        int priority = rand() % 2; // 0 or 1
        {
            lock_guard<mutex> lock(mtx);
            orderQueue.push(Order(orderId, priority));
            cout << "Waiter " << id << " placed Order "
                 << orderId
                 << (priority ? " [VIP]" : " [Normal]") << endl;
        }
        cv.notify_one(); // notify chef
    }
}

void chef(int id) {
    while (true) {
        kitchenSlots.acquire(); // wait for available slot in kitchen

        unique_lock<mutex> lock(mtx);

        cv.wait(lock, [] {
            return !orderQueue.empty() || finished;
        });

        if (orderQueue.empty() && finished) {
            kitchenSlots.release();
            return;
        }

        Order order = orderQueue.top();
        orderQueue.pop();
        activeChefs++;
        cout << "Chef " << id << " is preparing Order "
             << order.id << endl;

        lock.unlock();

        // simulate cooking
        this_thread::sleep_for(chrono::milliseconds(1000));
        activeChefs--;
        completedOrders++;
        cout << "Chef " << id << " completed Order "
             << order.id << endl;

        kitchenSlots.release();
    }
}
//monitoring thread

void monitor(){
    while(!finished) {
      {
          lock_guard<mutex> lock(mtx);
          cout << "\n--- Kitchen Status ---\n";
            cout << "Orders in Queue: " << orderQueue.size() << endl;
            cout << "Active Chefs: " << activeChefs << endl;
            cout << "Completed Orders: " << completedOrders << endl;
            cout << "----------------------\n";
      }
      this_thread::sleep_for(chrono::seconds(2));
    }
}      
int main() {
    srand(time(0));

    vector<thread> waiters;
    vector<thread> chefs;
   
    // create waiters
    for (int i = 0; i < 3; i++) {
        waiters.push_back(thread(waiter, i + 1));
    }

    // create chefs
    for (int i = 0; i < 5; i++) {
        chefs.push_back(thread(chef, i + 1));
    }
 thread monitorThread(monitor);
    // wait for waiters
    for (auto &w : waiters) {
        w.join();
    }

    // signal finish
    {
        lock_guard<mutex> lock(mtx);
        finished = true;
    }

    cv.notify_all();

    // wait for chefs
    for (auto &c : chefs) {
        c.join();
    }
    //wait for monitor
    monitorThread.join();
    cout << "\nAll orders processed!\n";
cout << "\n========= FINAL REPORT =========\n";
cout << "Total Orders: " << orderCounter << endl;
cout << "Completed Orders: " << completedOrders << endl;
cout << "Remaining Orders: " << orderQueue.size() << endl;
cout << "================================\n";
    return 0;
}
       

