# **SyncText \- Design Document**

## **NAME:- Sayan Das**

**ROLL NUMBER:- 25CS60R01**

## **1\. System Architecture**

### **High-Level Overview**

SyncText is a CRDT-based collaborative text editor that enables multiple users to edit documents simultaneously while maintaining eventual consistency without locks. The system uses a distributed architecture where each user runs a local instance that communicates via message queues.

### **Major Components and Interactions**

**Core Components:**

1. **File Monitor** \- Watches local file for changes  
2. **Change Detector** \- Identifies and creates update objects  
3. **Message Queue Manager** \- Handles inter-process communication  
4. **CRDT Merger** \- Resolves conflicts and merges operations  
5. **User Registry** \- Tracks active users in shared memory  
6. **Display Manager** \- Updates terminal interface

## **2\. Implementation Details**

### **Change Detection Implementation**

**File Monitoring Approach:**

* Uses `stat()` system call to check file modification timestamps  
* Polls every 2 seconds for changes  
* Maintains previous state in memory for comparison  
* Line-by-line diffing to identify specific changes

**Change Identification Logic:**

1\. OP\_INSERT: prev\_lines\[i\] \== NULL && lines\[i\] \!= NULL  
2\. OP\_DELETE: prev\_lines\[i\] \!= NULL && lines\[i\] \== NULL    
3\. OP\_REPLACE: Both exist but content differs

### **Message Queue and Shared Memory Structure**

**Shared Memory Registry:**

* Fixed-size array of `UserInfo` structures (MAX\_USERS \= 5\)  
* Protected by semaphore for concurrent access  
* Contains: user\_id, queue\_name, active status, heartbeat timestamp

**Message Queues:**

* POSIX message queues with names like `/queue_user_1`  
* Each queue stores `UpdateObject` structures  
* Supports 10 messages maximum per queue

### **CRDT Merge Algorithm Implementation**

**Conflict Detection:**

c  
int operations\_conflict(const struct UpdateObject \*op1, const struct UpdateObject \*op2) {  
    return (op1-\>line\_num \== op2-\>line\_num); // Simplified conflict detection  
}

**Last-Writer-Wins Resolution:**

1. **Primary:** Compare timestamps (latest wins)  
2. **Secondary:** Compare user\_id (lexicographically smaller wins)  
3. **Tertiary:** Compare sequence\_id (higher wins for same user)

**Merge Process:**

1. Collect all local and received operations  
2. Sort operations deterministically (timestamp → user\_id → sequence\_id)  
3. Detect conflicts between operations on same line  
4. Apply non-conflicting operations  
5. Apply winning operations from conflicts  
6. Clear buffers and save merged state

### **Thread Architecture**

**Four Concurrent Threads:**

1. **Main Thread:** CRDT loop, display updates, merge triggering  
2. **Monitor Thread:** File change detection (2-second intervals)  
3. **Listener Thread:** Message queue reception (1-second timeout)  
4. **Heartbeat Thread:** Registry updates (3-second intervals)

**Thread Communication:**

* Shared buffers for local and received operations  
* Mutex-protected merge operations  
* Atomic flags for thread coordination

## **3\. Design Decisions and Rationale**

### **Lock-Free Operation Strategy**

**Why CRDT Instead of Locks:**

* Mathematical properties guarantee convergence without coordination  
* Better performance in distributed environment  
* Eliminates deadlock and livelock risks  
* Aligns with modern collaborative editing trends

**Atomic Operations Used:**

* Message queue operations are inherently atomic  
* Semaphore-protected shared memory access  
* Mutex for merge operation (brief, localized critical section)

### **CRDT Implementation Choices**

**Operation Granularity:**

* **Chosen:** Line-level operations for simplicity  
* **Alternative:** Character-level operations (more complex but finer granularity)  
* **Rationale:** Line-level simplifies conflict detection while maintaining CRDT properties

**Merge Triggering:**

* **Chosen:** After N=5 operations OR when received operations exist  
* **Rationale:** Balances responsiveness with performance  
* **Alternative:** Immediate merging (higher overhead)

### **Data Structure Trade-offs**

**Line Storage:**

* **Chosen:** Fixed array of string pointers (MAX\_LINES \= 1000\)  
* **Pros:** Simple, predictable memory usage  
* **Cons:** Limited document size, potential memory waste

**Operation Buffers:**

* **Chosen:** Fixed-size arrays for local and received operations  
* **Pros:** No dynamic allocation overhead  
* **Cons:** Fixed capacity limits

## **4\. Challenges and Solutions**

### **Major Implementation Challenges**

**1\. Change Detection Accuracy**

* **Problem:** Detecting exact changes between file versions  
* **Solution:** Maintain complete previous state and compare line-by-line  
* **Limitation:** Only detects line-level changes, not character-level

**2\. Dynamic User Management**

* **Problem:** Users joining/leaving during operation  
* **Solution:** Heartbeat system with timeout detection  
* **Registry Cleanup:** Periodic checks for inactive users

**3\. Message Queue Reliability**

* **Problem:** Ensuring reliable delivery between processes  
* **Solution:** POSIX message queues with proper error handling  
* **Recovery:** Queue creation/deletion on startup/shutdown

**4\. CRDT Conflict Resolution**

* **Problem:** Ensuring deterministic merging across all users  
* **Solution:** Comprehensive LWW strategy with multiple tie-breakers  
* **Verification:** Extensive testing with concurrent edits

### **Debugging Strategies**

**Logging and Visualization:**

* Extensive printf debugging with operation details  
* Visual display of merge process steps  
* Operation sequence tracking per user

**Testing Approach:**

* Manual testing with multiple terminal instances  
* Scripted scenarios for conflict cases  
* Valgrind for memory leak detection

### **Performance Optimizations**

**Polling Intervals:**

* File monitoring: 2 seconds (responsive but not excessive)  
* Heartbeat: 3 seconds (balance between freshness and overhead)  
* Display updates: 1 second (smooth user experience)

**Buffer Management:**

* Operation batching (N=5) reduces communication overhead  
* Fixed buffers prevent unbounded memory growth  
* Early merge triggering with received operations

## **5\. CRDT Properties Implementation**

### **Commutativity**

* Operations sorted deterministically before application  
* Merge order doesn't affect final result

### **Associativity**

* Grouping of operations handled through buffering strategy  
* All operations eventually applied regardless of grouping

### **Idempotency**

* Operation sequence tracking prevents duplicate application  
* Clear separation between applied and pending operations

## **6\. Limitations and Future Improvements**

### **Current Limitations**

1. Line-level granularity (not character-level)  
2. Fixed maximum document size (1000 lines)  
3. No persistent operation history  
4. Basic conflict detection (line-based only)

### **Potential Enhancements**

1. Character-level CRDT operations  
2. Operational transformation for richer semantics  
3. Persistent storage and operation replay  
4. Web-based interface instead of terminal  
5. Advanced conflict visualization for users