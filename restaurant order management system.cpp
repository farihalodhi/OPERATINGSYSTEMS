/*
 * Restaurant Order Management System with Terminal GUI
 * =====================================================
 * OS Concepts Demonstrated:
 *  1. Multithreading           - waiters, chefs, monitor run as concurrent threads
 *  2. Mutex / Locks            - protects shared queue & log file
 *  3. Condition Variables      - chefs wait/signal on queue state
 *  4. Semaphore                - kitchenSlots limits concurrent chefs (bounded resource)
 *  5. Atomic Variables         - lock-free counters (completedOrders, activeChefs, etc.)
 *  6. Producer-Consumer        - waiters produce orders, chefs consume them
 *  7. Priority Scheduling      - VIP orders served first via priority_queue
 *  8. Aging - normal orders gain priority over time
 *  9. Priority Inversion  - aging prevents low-priority orders blocking high ones
 * 10. Deadlock Avoidance       - consistent lock ordering, timed waits, no circular waits
 * 11. Bounded Buffer           - max queue size enforced; waiters block when full
 * 12. Order Timeout            - orders cancelled if waiting too long in queue
 * 13. Chef Specialization      - dedicated VIP chefs + general chefs
 * 14. File Logging             - all events written to restaurant_log.txt
 * 15. Dynamic Chef Allocation  - scaler thread spawns/retires chefs based on queue load
 * 16. Terminal GUI Dashboard   - ncurses-based real-time visualization
 */

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <semaphore>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <functional>
#include <deque>
#include <map>
#include <ncurses.h>
using namespace std;
using namespace chrono;

//  CONSTANTS
const int MAX_QUEUE_SIZE       = 10;
const int ORDERS_PER_WAITER    = 8;
const int NUM_WAITERS          = 3;
const int MIN_CHEFS            = 2;
const int MAX_CHEFS            = 7;
const int TIMEOUT_MS           = 4000;
const int AGING_THRESHOLD_MS   = 2000;
const int SCALE_UP_THRESHOLD   = 5;
const int SCALE_DOWN_THRESHOLD = 2;


//  ORDER STRUCT
struct Order {
    int    id;
    int    priority;
    int    effectivePriority;
    time_point<steady_clock> arrivalTime;

    Order(int i, int p)
        : id(i), priority(p), effectivePriority(p),
          arrivalTime(steady_clock::now()) {}

    long long waitingMs() const {
        return duration_cast<milliseconds>(
            steady_clock::now() - arrivalTime).count();
    }
};

//  PRIORITY COMPARATOR

struct Compare {
    bool operator()(const Order& a, const Order& b) {
        return a.effectivePriority < b.effectivePriority;
    }
};


//  GUI LOG ENTRY

struct LogEntry {
    string timestamp;
    string message;
    int    color;
};

//  CHEF STATUS

struct ChefStatus {
    int    id;
    bool   isVIP;
    bool   isActive;
    int    currentOrderId;
    string status;
};

//  SHARED RESOURCES
priority_queue<Order, vector<Order>, Compare> orderQueue;
mutex              mtx;
condition_variable cv;
condition_variable cvProducer;

atomic<int>  orderCounter(0);
atomic<bool> finished(false);
atomic<int>  completedOrders(0);
atomic<int>  cancelledOrders(0);
atomic<int>  activeChefs(0);
atomic<int>  vipCompleted(0);
atomic<int>  normalCompleted(0);

mutex          chefMtx;
vector<thread> dynamicChefThreads;
atomic<int>    totalChefs(0);
atomic<bool>   chefRetireFlag(false);

counting_semaphore<3> kitchenSlots(3);


//  GUI STATE

mutex guiMtx;
deque<LogEntry> eventLog;
const int MAX_LOG_ENTRIES = 50;
map<int, ChefStatus> chefStatuses;

// File logger
ofstream logFile;
mutex    logMtx;


//  LOGGING FUNCTIONS

string getTimestamp() {
    auto now = system_clock::now();
    auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    time_t t = system_clock::to_time_t(now);
    tm* ltm  = localtime(&t);

    ostringstream ts;
    ts << setfill('0') << setw(2) << ltm->tm_hour << ":"
       << setw(2) << ltm->tm_min  << ":"
       << setw(2) << ltm->tm_sec;
    return ts.str();
}

void logEvent(const string& msg, int color = 1) {
    string ts = getTimestamp();
    {
        lock_guard<mutex> lk(guiMtx);
        eventLog.push_back({ts, msg, color});
        if ((int)eventLog.size() > MAX_LOG_ENTRIES)
            eventLog.pop_front();
    }
    lock_guard<mutex> lk(logMtx);
    if (logFile.is_open())
        logFile << "[" << ts << "] " << msg << "\n";
}

void updateChefStatus(int chefId, bool isVIP, const string& status,
                      bool isActive = false, int orderId = -1) {
    lock_guard<mutex> lk(guiMtx);
    chefStatuses[chefId] = {chefId, isVIP, isActive, orderId, status};
}

WINDOW *headerWin, *mainWin, *logWin;

void initGUI() {
    
    setlocale(LC_ALL, "");
    initscr();
    start_color();
    use_default_colors();
    cbreak();
    noecho();
    curs_set(0);

    // Color pairs
    init_pair(1, COLOR_WHITE,   -1);   // Normal
    init_pair(2, COLOR_YELLOW,  -1);   // VIP
    init_pair(3, COLOR_RED,     -1);   // Warning
    init_pair(4, COLOR_GREEN,   -1);   // Success
    init_pair(5, COLOR_CYAN,    -1);   // Info
    init_pair(6, COLOR_MAGENTA, -1);   // Header

    int maxY, maxX;
    getmaxyx(stdscr, maxY, maxX);

    headerWin = newwin(3,    maxX,  0, 0);
    mainWin   = newwin(14,   maxX,  3, 0);
    logWin    = newwin(maxY - 17, maxX, 17, 0);
    scrollok(logWin, TRUE);
}

void renderGUI() {

    vector<Order> queueSnapshot;
    int qsize = 0;
    {
        lock_guard<mutex> lk(mtx);
        priority_queue<Order, vector<Order>, Compare> tmp = orderQueue;
        qsize = (int)tmp.size();
        while (!tmp.empty() && (int)queueSnapshot.size() < 6) {
            queueSnapshot.push_back(tmp.top());
            tmp.pop();
        }
    }
    int total     = orderCounter.load();
    int completed = completedOrders.load();
    int cancelled = cancelledOrders.load();
    int vip       = vipCompleted.load();
    int normal    = normalCompleted.load();
    int achefs    = activeChefs.load();
    int tchefs    = totalChefs.load();

    // Snapshot GUI state
    vector<ChefStatus>  chefSnap;
    vector<LogEntry>    logSnap;
    {
        lock_guard<mutex> lk(guiMtx);
        for (auto& [id, cs] : chefStatuses)
            chefSnap.push_back(cs);
        logSnap = vector<LogEntry>(eventLog.begin(), eventLog.end());
    }

    int maxY, maxX;
    getmaxyx(stdscr, maxY, maxX);

    // ---- HEADER ----
    werase(headerWin);
    wattron(headerWin, COLOR_PAIR(6) | A_BOLD | A_REVERSE);
    for (int col = 0; col < maxX; col++) mvwaddch(headerWin, 0, col, ' ');
    mvwprintw(headerWin, 0, 2, "  RESTAURANT ORDER MANAGEMENT SYSTEM  |  Live Dashboard");
    wattroff(headerWin, COLOR_PAIR(6) | A_BOLD | A_REVERSE);
    wattron(headerWin, COLOR_PAIR(5));
    mvwprintw(headerWin, 1, 2, "Orders: %d  |  Completed: %d  |  Cancelled: %d  |  Chefs: %d/%d active",
              total, completed, cancelled, achefs, tchefs);
    wattroff(headerWin, COLOR_PAIR(5));
    mvwprintw(headerWin, 2, 2, "-------------------------------------------------------------");
    wrefresh(headerWin);

    // ---- MAIN PANEL ----
    werase(mainWin);
    int half = maxX / 2;


    wattron(mainWin, A_BOLD);
    mvwprintw(mainWin, 0, 2, "[ ORDER QUEUE %d/%d ]", qsize, MAX_QUEUE_SIZE);
    wattroff(mainWin, A_BOLD);
    mvwprintw(mainWin, 1, 2, "%-5s %-10s %s", "Type", "Order", "Wait");
    mvwprintw(mainWin, 2, 2, "-------------------------");

    if (queueSnapshot.empty()) {
        wattron(mainWin, COLOR_PAIR(5));
        mvwprintw(mainWin, 3, 2, "(queue empty)");
        wattroff(mainWin, COLOR_PAIR(5));
    } else {
        int row = 3;
        for (auto& o : queueSnapshot) {
            bool isVIP = (o.effectivePriority == 1);
            wattron(mainWin, COLOR_PAIR(isVIP ? 2 : 1));
            mvwprintw(mainWin, row, 2, "%-5s Order #%-4d %4lldms",
                      isVIP ? "[VIP]" : "[NRM]", o.id, o.waitingMs());
            wattroff(mainWin, COLOR_PAIR(isVIP ? 2 : 1));
            row++;
        }
    }

    // Right column: Stats
    wattron(mainWin, A_BOLD);
    mvwprintw(mainWin, 0, half + 2, "[ STATS ]");
    wattroff(mainWin, A_BOLD);

    mvwprintw(mainWin, 1, half + 2, "Total Orders  : %d", total);
    mvwprintw(mainWin, 2, half + 2, "Completed     : %d", completed);

    wattron(mainWin, COLOR_PAIR(2));
    mvwprintw(mainWin, 3, half + 2, "  VIP         : %d", vip);
    wattroff(mainWin, COLOR_PAIR(2));

    mvwprintw(mainWin, 4, half + 2, "  Normal      : %d", normal);

    wattron(mainWin, COLOR_PAIR(3));
    mvwprintw(mainWin, 5, half + 2, "Cancelled     : %d", cancelled);
    wattroff(mainWin, COLOR_PAIR(3));

    mvwprintw(mainWin, 6, half + 2, "Active Chefs  : %d / %d", achefs, tchefs);


    for (int r = 0; r < 13; r++)
        mvwaddch(mainWin, r, half, '|');

    wattron(mainWin, A_BOLD);
    mvwprintw(mainWin, 8, 2, "[ CHEF ACTIVITY ]");
    wattroff(mainWin, A_BOLD);
    mvwprintw(mainWin, 9, 2, "%-4s %-9s %s", "ID", "Type", "Status");
    mvwprintw(mainWin, 10, 2, "--------------------------------------------");

    int crow = 11;
    for (auto& cs : chefSnap) {
        if (crow >= 14) break;
        int color = cs.isActive ? 4 : (cs.isVIP ? 2 : 1);
        wattron(mainWin, COLOR_PAIR(color));
        string typeStr = cs.isVIP ? "VIP Chef " : "Chef     ";
        mvwprintw(mainWin, crow, 2, "%-4d %-9s %s",
                  cs.id, typeStr.c_str(), cs.status.c_str());
        wattroff(mainWin, COLOR_PAIR(color));
        crow++;
    }

    wrefresh(mainWin);

    // ---- EVENT LOG ----
    werase(logWin);
    int logHeight = getmaxy(logWin);
    wattron(logWin, A_BOLD);
    mvwprintw(logWin, 0, 2, "[ EVENT LOG ]");
    wattroff(logWin, A_BOLD);

    int maxRows = logHeight - 2;
    int startIdx = max(0, (int)logSnap.size() - maxRows);
    int lrow = 1;
    for (int i = startIdx; i < (int)logSnap.size(); i++) {
        auto& e = logSnap[i];
        wattron(logWin, COLOR_PAIR(e.color));
        mvwprintw(logWin, lrow, 2, "[%s] %s", e.timestamp.c_str(), e.message.c_str());
        wattroff(logWin, COLOR_PAIR(e.color));
        lrow++;
    }

    wrefresh(logWin);
}

void guiThread() {
    initGUI();
    while (!finished) {
        renderGUI();
        this_thread::sleep_for(milliseconds(150)); // ~7 FPS - enough and not heavy
    }
    renderGUI();
    this_thread::sleep_for(seconds(2));

    delwin(headerWin);
    delwin(mainWin);
    delwin(logWin);
    endwin();
}

//  AGING THREAD

void agingDaemon() {
    while (!finished) {
        this_thread::sleep_for(milliseconds(500));
        lock_guard<mutex> lock(mtx);
        if (orderQueue.empty()) continue;

        vector<Order> temp;
        bool anyPromoted = false;

        while (!orderQueue.empty()) {
            Order o = orderQueue.top(); orderQueue.pop();
            if (o.priority == 0 && o.effectivePriority == 0
                && o.waitingMs() >= AGING_THRESHOLD_MS) {
                o.effectivePriority = 1;
                logEvent("AGING: Order #" + to_string(o.id)
                         + " promoted after " + to_string(o.waitingMs()) + "ms", 5);
                anyPromoted = true;
            }
            temp.push_back(o);
        }
        for (auto& o : temp) orderQueue.push(o);
        if (anyPromoted) cv.notify_all();
    }
}

//  TIMEOUT WATCHER

void timeoutWatcher() {
    while (!finished) {
        this_thread::sleep_for(milliseconds(500));
        lock_guard<mutex> lock(mtx);
        if (orderQueue.empty()) continue;

        vector<Order> keep;
        bool anyCancelled = false;

        while (!orderQueue.empty()) {
            Order o = orderQueue.top(); orderQueue.pop();
            if (o.waitingMs() > TIMEOUT_MS) {
                cancelledOrders++;
                logEvent("TIMEOUT: Order #" + to_string(o.id)
                         + " cancelled after " + to_string(o.waitingMs()) + "ms", 3);
                anyCancelled = true;
            } else {
                keep.push_back(o);
            }
        }
        for (auto& o : keep) orderQueue.push(o);
        if (anyCancelled) cvProducer.notify_all();
    }
}


//  CHEF THREAD

void chef(int id, bool vipOnly, bool dynamic = false) {
    string role = vipOnly ? "VIP Chef" : "Chef";
    totalChefs++;
    updateChefStatus(id, vipOnly, "Waiting for orders");

    while (true) {

        if (dynamic) {
            bool shouldRetire = false;
            {
                lock_guard<mutex> lock(mtx);
                if (chefRetireFlag && (int)orderQueue.size() <= SCALE_DOWN_THRESHOLD) {
                    shouldRetire = true;
                    chefRetireFlag = false;
                }
            }
            if (shouldRetire) {
                logEvent("SCALE-DOWN: Chef #" + to_string(id) + " retiring", 5);
                updateChefStatus(id, vipOnly, "Retired");
                totalChefs--;
                return;
            }
        }

        kitchenSlots.acquire();
        updateChefStatus(id, vipOnly, "Waiting for order");

        unique_lock<mutex> lock(mtx);
        cv.wait(lock, [&] {
            if (finished && orderQueue.empty()) return true;
            if (orderQueue.empty()) return false;
            if (vipOnly) return orderQueue.top().effectivePriority == 1;
            return true;
        });

        if (orderQueue.empty() && finished) {
            kitchenSlots.release();
            updateChefStatus(id, vipOnly, "Finished");
            totalChefs--;
            return;
        }

        if (vipOnly && (orderQueue.empty()
            || orderQueue.top().effectivePriority != 1)) {
            kitchenSlots.release();
            updateChefStatus(id, vipOnly, "Waiting (no VIP)");
            continue;
        }

        if (orderQueue.empty()) {
            kitchenSlots.release();
            continue;
        }

        Order order = orderQueue.top(); orderQueue.pop();
        activeChefs++;

        string orderType = order.effectivePriority ? "[VIP]" : "[NRM]";
        updateChefStatus(id, vipOnly, "Cooking Order #" + to_string(order.id), true, order.id);
        logEvent(role + " #" + to_string(id) + " picked Order #" + to_string(order.id)
                 + " " + orderType + " (waited " + to_string(order.waitingMs()) + "ms)",
                 order.effectivePriority ? 2 : 1);

        cvProducer.notify_one();
        lock.unlock();

        int cookTime = 600 + rand() % 800;
        this_thread::sleep_for(milliseconds(cookTime));

        activeChefs--;
        completedOrders++;
        if (order.priority == 1 || order.effectivePriority == 1)
            vipCompleted++;
        else
            normalCompleted++;

        logEvent(role + " #" + to_string(id) + " completed Order #"
                 + to_string(order.id) + " (" + to_string(cookTime) + "ms)", 4);
        updateChefStatus(id, vipOnly, "Idle");

        kitchenSlots.release();
    }
}


//  DYNAMIC CHEF SCALER

void chefScaler() {
    int dynamicChefId = 100;
    while (!finished) {
        this_thread::sleep_for(milliseconds(800));

        int queueSize;
        {
            lock_guard<mutex> lock(mtx);
            queueSize = (int)orderQueue.size();
        }

        int current = totalChefs.load();

        if (queueSize > SCALE_UP_THRESHOLD && current < MAX_CHEFS) {
            int newId = ++dynamicChefId;
            logEvent("SCALE-UP: Spawning Chef #" + to_string(newId)
                     + " (queue=" + to_string(queueSize) + ")", 5);
            lock_guard<mutex> lk(chefMtx);
            dynamicChefThreads.emplace_back(thread(chef, newId, false, true));
        } else if (queueSize < SCALE_DOWN_THRESHOLD && current > MIN_CHEFS) {
            chefRetireFlag = true;
            cv.notify_all();
        }
    }
}

//  WAITER THREAD

void waiter(int id) {
    for (int i = 0; i < ORDERS_PER_WAITER; i++) {
        this_thread::sleep_for(milliseconds(rand() % 500 + 100));

        int orderId  = ++orderCounter;
        int priority = rand() % 2;

        {
            unique_lock<mutex> lock(mtx);
            cvProducer.wait(lock, [] {
                return (int)orderQueue.size() < MAX_QUEUE_SIZE || finished.load();
            });
            if (finished) return;

            orderQueue.push(Order(orderId, priority));
            string orderType = priority ? "[VIP]" : "[NRM]";
            logEvent("Waiter #" + to_string(id) + " placed Order #" + to_string(orderId)
                     + " " + orderType + " (queue=" + to_string(orderQueue.size()) + ")",
                     priority ? 2 : 1);
        }
        cv.notify_one();
    }
    logEvent("Waiter #" + to_string(id) + " finished shift", 5);
}
//MAIN
int main() {
    srand(time(0));

    logFile.open("restaurant_log.txt", ios::out | ios::trunc);
    if (!logFile.is_open()) {
        cerr << "Warning: could not open log file.\n";
        return 1;
    }

    logEvent("===== Restaurant Simulation Started =====", 6);
    logEvent("Config: " + to_string(NUM_WAITERS) + " waiters, "
             + to_string(MIN_CHEFS) + "-" + to_string(MAX_CHEFS) + " chefs", 5);

    vector<thread> waiters, baseChefs;

    thread gui(guiThread);
    this_thread::sleep_for(milliseconds(500));

    // Base chefs: 2 VIP, 2 general
    for (int i = 1; i <= 4; i++) {
        bool vip = (i <= 2);
        baseChefs.push_back(thread(chef, i, vip, false));
    }

    thread agingThread(agingDaemon);
    thread timeoutThread(timeoutWatcher);
    thread scalerThread(chefScaler);

    for (int i = 1; i <= NUM_WAITERS; i++)
        waiters.push_back(thread(waiter, i));

    for (auto& w : waiters) w.join();
    logEvent("All waiters finished. Draining kitchen...", 5);


    while (true) {
        { lock_guard<mutex> lock(mtx); if (orderQueue.empty()) break; }
        this_thread::sleep_for(milliseconds(200));
    }

    finished = true;
    cv.notify_all();
    cvProducer.notify_all();

    for (auto& c : baseChefs) c.join();

    {
        lock_guard<mutex> lk(chefMtx);
        for (auto& t : dynamicChefThreads) if (t.joinable()) t.join();
    }

    agingThread.join();
    timeoutThread.join();
    scalerThread.join();

    logEvent("===== SIMULATION COMPLETE =====", 6);
    logEvent("Total: " + to_string(orderCounter.load()) + " orders", 4);
    logEvent("Completed: " + to_string(completedOrders.load())
             + " (VIP: " + to_string(vipCompleted.load())
             + ", Normal: " + to_string(normalCompleted.load()) + ")", 4);
    logEvent("Cancelled: " + to_string(cancelledOrders.load()), 3);

    gui.join();

    if (logFile.is_open()) logFile.close();

    cout << "\n\n=======================================\n";
    cout << "Simulation complete!\n";
    cout << "Full log saved to: restaurant_log.txt\n";
    cout << "=======================================\n\n";

    return 0;
}
