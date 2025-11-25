#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <signal.h>
#include <mqueue.h>
#include <errno.h>

#define MAX_LINES 1000
#define MAX_LEN 1024
#define MAX_OPERATIONS 5
#define MAX_MESSAGE_SIZE 2048

char user_id[50];
char filename[100];
char *lines[MAX_LINES];
char *prev_lines[MAX_LINES];
time_t last_mod_time = 0;

#define MAX_USERS 5
#define SHM_NAME "/synctext_registry"

// Operation types
typedef enum {
    OP_INSERT,
    OP_DELETE,
    OP_REPLACE
} OperationType;

// Update object with CRDT properties
struct UpdateObject {
    OperationType type;
    int line_num;
    int start_col;
    int end_col;
    char old_content[MAX_LEN];
    char new_content[MAX_LEN];
    time_t timestamp;
    char user_id[50];
    int sequence_id; // For ordering operations from same user
};

// User registry
struct UserInfo {
    char user_id[32];
    char queue_name[64];
    int active;
    time_t last_heartbeat;
    int last_sequence; // Track last sequence from each user
};

void save_current_state();

struct UserInfo *registry;
int shm_fd;
sem_t *registry_sem;
volatile sig_atomic_t running = 1;

// Message queue and buffers
mqd_t my_queue;
struct UpdateObject local_operations[MAX_OPERATIONS];
struct UpdateObject received_operations[MAX_OPERATIONS * MAX_USERS];
int local_op_count = 0;
int received_op_count = 0;

// CRDT state for each user
int user_sequence = 0;
pthread_mutex_t merge_mutex = PTHREAD_MUTEX_INITIALIZER;

// Idempotency log
#define APPLIED_LOG_SIZE 1024
struct AppliedId { char user_id[50]; int sequence_id; } applied_log[APPLIED_LOG_SIZE];
int applied_head = 0;
int applied_size = 0;

static int already_applied(const struct UpdateObject *op) {
    for (int k = 0; k < applied_size; k++) {
        int idx = (applied_head - 1 - k + APPLIED_LOG_SIZE) % APPLIED_LOG_SIZE;
        if (applied_log[idx].sequence_id == op->sequence_id &&
            strcmp(applied_log[idx].user_id, op->user_id) == 0) return 1;
    }
    return 0;
}

static void mark_applied(const struct UpdateObject *op) {
    strncpy(applied_log[applied_head].user_id, op->user_id, sizeof(applied_log[applied_head].user_id)-1);
    applied_log[applied_head].user_id[sizeof(applied_log[applied_head].user_id)-1] = '\0';
    applied_log[applied_head].sequence_id = op->sequence_id;
    applied_head = (applied_head + 1) % APPLIED_LOG_SIZE;
    if (applied_size < APPLIED_LOG_SIZE) applied_size++;
}


/*
int operations_conflict(const struct UpdateObject *op1, const struct UpdateObject *op2) {
    if (op1->line_num != op2->line_num) {
        return 0; // Different lines, no conflict
    }
    
    // For CRDT, we consider operations on the same line as potentially conflicting
    // In a real implementation, we'd check character positions, but for simplicity:
    return 1;
}
*/

// Two ops conflict iff: same line AND overlapping [start_col, end_col)
static inline int ranges_overlap(int a1, int a2, int b1, int b2) {
    // treat empty end as start for safety
    if (a2 < a1) a2 = a1;
    if (b2 < b1) b2 = b1;
    return (a1 < b2) && (b1 < a2);
}

int operations_conflict(const struct UpdateObject *op1, const struct UpdateObject *op2) {
    if (op1->line_num != op2->line_num) return 0;
    return ranges_overlap(op1->start_col, op1->end_col, op2->start_col, op2->end_col);
}


// CRDT: Last-Writer-Wins resolution
int resolve_conflict_crdt(const struct UpdateObject *op1, const struct UpdateObject *op2) {
    // Primary: Compare timestamps
    if (op1->timestamp > op2->timestamp) {
        return 1;
    } else if (op1->timestamp < op2->timestamp) {
        return -1;
    }
    
    // Secondary: If timestamps are equal, compare user IDs
    if (strcmp(op1->user_id, op2->user_id) < 0) {
        return 1;
    } else if (strcmp(op1->user_id, op2->user_id) > 0) {
        return -1;
    }
    
    // Tertiary: If same user, compare sequence numbers
    return (op1->sequence_id > op2->sequence_id) ? 1 : -1;
}

// CRDT: Apply operation with idempotency check
void apply_operation_crdt(const struct UpdateObject *op) {
    if (op->line_num < 0 || op->line_num >= MAX_LINES) return;
    
    printf("CRDT Applying: user=%s, line=%d, type=%d, seq=%d\n", 
           op->user_id, op->line_num, op->type, op->sequence_id);
    
    switch (op->type) {
        /*
        case OP_INSERT:
            if (lines[op->line_num] == NULL) {
                lines[op->line_num] = strdup(op->new_content);
            } else {
                // Simple line replacement for this implementation
                free(lines[op->line_num]);
                lines[op->line_num] = strdup(op->new_content);
            }
            break;
        */
    case OP_INSERT:
    {
        // Rebuild from old line using the window. We stored full new_content for simplicity,
        // so just replace the window in the current local line.
        const char *cur = lines[op->line_num] ? lines[op->line_num] : "";
        int cur_len = (int)strlen(cur);
        int a = op->start_col, b = op->end_col;
        if (a < 0)
            a = 0;
        if (b < a)
            b = a;
        if (b > cur_len)
            b = cur_len;

        size_t pre_len = a;
        size_t mid_len = strlen(op->new_content); // using full new line or you can store only the slice
        size_t suf_len = cur_len - b;

        char *buf = malloc(pre_len + mid_len + suf_len + 1);
        memcpy(buf, cur, pre_len);
        memcpy(buf + pre_len, op->new_content + a, mid_len - a - (cur_len - b)); // simplified fallback: use full new_content
        memcpy(buf + pre_len + mid_len, cur + b, suf_len);
        buf[pre_len + mid_len + suf_len] = '\0';

        if (lines[op->line_num])
            free(lines[op->line_num]);
        lines[op->line_num] = buf;
        break;
    }

        case OP_DELETE:
            if (lines[op->line_num] != NULL) {
                free(lines[op->line_num]);
                lines[op->line_num] = NULL;
            }
            break;
            
        case OP_REPLACE:
            if (lines[op->line_num] != NULL) {
                free(lines[op->line_num]);
                lines[op->line_num] = strdup(op->new_content);
            }
            break;
    }
}

// Save document to file
void save_document_to_file() {
    FILE *fp = fopen(filename, "w");
    if (!fp) {
        perror("fopen");
        return;
    }
    
    for (int i = 0; i < MAX_LINES; i++) {
        if (lines[i] != NULL) {
            fprintf(fp, "%s\n", lines[i]);
        }
    }
    
    fclose(fp);
    
    // Update modification time
    struct stat attr;
    if (stat(filename, &attr) == 0) {
        last_mod_time = attr.st_mtime;
    }
}

// CRDT: Proper merge algorithm using true CRDT properties
void merge_operations_crdt() {
    pthread_mutex_lock(&merge_mutex);
    
    printf("\n=== CRDT MERGE STARTED ===\n");
    printf("Local ops: %d, Received ops: %d\n", local_op_count, received_op_count);
    
    if (local_op_count == 0 && received_op_count == 0) {
        printf("No operations to merge\n");
        pthread_mutex_unlock(&merge_mutex);
        return;
    }
    
    // Combine all operations
    struct UpdateObject all_ops[local_op_count + received_op_count];
    int total_ops = 0;
    
    // Add local operations
    for (int i = 0; i < local_op_count; i++) {
        all_ops[total_ops++] = local_operations[i];
    }
    
    // Add received operations
    for (int i = 0; i < received_op_count; i++) {
        all_ops[total_ops++] = received_operations[i];
    }
    
    printf("Total operations to merge: %d\n", total_ops);
    
    // CRDT Property: Commutativity - Sort operations deterministically
    // Sort by timestamp, then user_id, then sequence_id
    for (int i = 0; i < total_ops - 1; i++) {
        for (int j = i + 1; j < total_ops; j++) {
            int swap = 0;
            
            if (all_ops[i].timestamp > all_ops[j].timestamp) {
                swap = 1;
            } else if (all_ops[i].timestamp == all_ops[j].timestamp) {
                if (strcmp(all_ops[i].user_id, all_ops[j].user_id) > 0) {
                    swap = 1;
                } else if (strcmp(all_ops[i].user_id, all_ops[j].user_id) == 0) {
                    if (all_ops[i].sequence_id > all_ops[j].sequence_id) {
                        swap = 1;
                    }
                }
            }
            
            if (swap) {
                struct UpdateObject temp = all_ops[i];
                all_ops[i] = all_ops[j];
                all_ops[j] = temp;
            }
        }
    }
    
    // Apply operations with conflict resolution
    int applied_count = 0;
    
    for (int i = 0; i < total_ops; i++) {
        if (already_applied(&all_ops[i])) continue;

        int should_apply = 1;
        
        // Check for conflicts with operations that will be applied later
        for (int j = i + 1; j < total_ops; j++) {
            if (operations_conflict(&all_ops[i], &all_ops[j])) {
                // Conflict detected - use LWW to resolve
                if (resolve_conflict_crdt(&all_ops[i], &all_ops[j]) == -1) {
                    // Current operation loses to a later one
                    should_apply = 0;
                    printf("Conflict: %s(%ld) loses to %s(%ld) on line %d\n",
                           all_ops[i].user_id, all_ops[i].timestamp,
                           all_ops[j].user_id, all_ops[j].timestamp,
                           all_ops[i].line_num);
                    break;
                }
            }
        }
        
        if (should_apply) {
            apply_operation_crdt(&all_ops[i]);
            mark_applied(&all_ops[i]); 
            applied_count++;
        }
    }
    
    printf("Applied %d/%d operations\n", applied_count, total_ops);
    
    // CRDT Property: Idempotency - Clear buffers after successful application
    local_op_count = 0;
    received_op_count = 0;
    
    // Save merged state
    save_document_to_file();
    save_current_state();
    
    printf("=== CRDT MERGE COMPLETED ===\n\n");
    
    pthread_mutex_unlock(&merge_mutex);
}

void initialize_registry() {
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        exit(1);
    }
    
    if (ftruncate(shm_fd, sizeof(struct UserInfo) * MAX_USERS) == -1) {
        perror("ftruncate");
        exit(1);
    }
    
    registry = mmap(NULL, sizeof(struct UserInfo) * MAX_USERS, 
                   PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (registry == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    
    registry_sem = sem_open("/registry_sem", O_CREAT, 0666, 1);
    if (registry_sem == SEM_FAILED) {
        perror("sem_open");
        exit(1);
    }
    
    sem_wait(registry_sem);
    int all_empty = 1;
    for (int i = 0; i < MAX_USERS; i++) {
        if (registry[i].active) {
            all_empty = 0;
            break;
        }
    }
    if (all_empty) {
        memset(registry, 0, sizeof(struct UserInfo) * MAX_USERS);
    }
    sem_post(registry_sem);
}

void create_message_queue() {
    char queue_name[64];
    snprintf(queue_name, sizeof(queue_name), "/queue_%s", user_id);
    
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(struct UpdateObject);
    attr.mq_curmsgs = 0;
    
    my_queue = mq_open(queue_name, O_CREAT | O_RDWR, 0666, &attr);
    if (my_queue == (mqd_t)-1) {
        perror("mq_open");
        exit(1);
    }
    
    printf("Message queue created: %s\n", queue_name);
}

void broadcast_update(const struct UpdateObject *update) {
    sem_wait(registry_sem);
    
    int sent_count = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (registry[i].active && strcmp(registry[i].user_id, user_id) != 0) {
            mqd_t target_queue = mq_open(registry[i].queue_name, O_WRONLY);
            if (target_queue != (mqd_t)-1) {
                if (mq_send(target_queue, (const char*)update, sizeof(struct UpdateObject), 0) == 0) {
                    sent_count++;
                }
                mq_close(target_queue);
            }
        }
    }
    
    sem_post(registry_sem);
    printf("Broadcasted to %d users\n", sent_count);
}

/*
void broadcast_accumulated_operations() {
    if (local_op_count == 0) return;
    
    printf("Broadcasting %d local operations\n", local_op_count);
    
    for (int i = 0; i < local_op_count; i++) {
        broadcast_update(&local_operations[i]);
    }
}
*/

void broadcast_accumulated_operations() {
    if (local_op_count == 0) return;
    printf("Broadcasting %d local operations\n", local_op_count);
    for (int i = 0; i < local_op_count; i++) {
        broadcast_update(&local_operations[i]);
    }
    // IMPORTANT: clear after send to prevent duplicates
    local_op_count = 0;
}

void detect_changes() {
    int changes_found = 0;
    
    for (int i = 0; i < MAX_LINES; i++) {
        int line_changed = 0;
        OperationType op_type;
        char *old_content = NULL;
        char *new_content = NULL;
        
        if (prev_lines[i] == NULL && lines[i] != NULL) {
            // Line inserted
            line_changed = 1;
            op_type = OP_INSERT;
            old_content = "";
            new_content = lines[i];
        } else if (prev_lines[i] != NULL && lines[i] == NULL) {
            // Line deleted
            line_changed = 1;
            op_type = OP_DELETE;
            old_content = prev_lines[i];
            new_content = "";
        } else if (prev_lines[i] != NULL && lines[i] != NULL && 
                   strcmp(prev_lines[i], lines[i]) != 0) {
            // Line modified
            line_changed = 1;
            op_type = OP_REPLACE;
            old_content = prev_lines[i];
            new_content = lines[i];
        }
        
        if (line_changed && local_op_count < MAX_OPERATIONS) {
            /*struct UpdateObject *op = &local_operations[local_op_count];
            op->type = op_type;
            op->line_num = i;
            op->timestamp = time(NULL);
            strcpy(op->user_id, user_id);
            op->sequence_id = ++user_sequence;
            
            if (old_content) strncpy(op->old_content, old_content, MAX_LEN-1);
            if (new_content) strncpy(op->new_content, new_content, MAX_LEN-1);
            
            op->start_col = 0;
            if (old_content) op->end_col = strlen(old_content);
            else if (new_content) op->end_col = strlen(new_content);
            else op->end_col = 0;
            
            local_op_count++;
            changes_found = 1;
            
            printf("Detected change: line %d, type %d, seq %d\n", i, op_type, op->sequence_id);*/

            // Compute minimal change window [start_col, end_col)
            int start = 0, end_old = 0, end_new = 0;
            const char *old_s = old_content ? old_content : "";
            const char *new_s = new_content ? new_content : "";
            int len_old = (int)strlen(old_s);
            int len_new = (int)strlen(new_s);

            // find first differing index
            int minlen = len_old < len_new ? len_old : len_new;
            while (start < minlen && old_s[start] == new_s[start])
                start++;

            // find last differing index from the end
            int tail_old = len_old - 1, tail_new = len_new - 1;
            while (tail_old >= start && tail_new >= start && old_s[tail_old] == new_s[tail_new])
            {
                tail_old--;
                tail_new--;
            }
            end_old = tail_old + 1;
            end_new = tail_new + 1;

            struct UpdateObject *op = &local_operations[local_op_count];
            op->type = op_type;
            op->line_num = i;
            op->timestamp = time(NULL);
            strncpy(op->user_id, user_id, sizeof(op->user_id) - 1);
            op->user_id[sizeof(op->user_id) - 1] = '\0';
            op->sequence_id = ++user_sequence;

            // carry full old/new line images so apply can reconstruct (simple model)
            op->old_content[0] = '\0';
            op->new_content[0] = '\0';
            if (old_content)
                strncpy(op->old_content, old_content, MAX_LEN - 1);
            if (new_content)
                strncpy(op->new_content, new_content, MAX_LEN - 1);

            // set precise column window
            op->start_col = start;
            op->end_col = end_old; // the range in the *old* text being replaced (delete=equal start==end_new)

            // If the line was fully inserted/deleted, the above reduces to [0, len_old] or [0,0] as appropriate.

            local_op_count++;
            printf("Detected change: line %d, [%d,%d), type %d, seq %d\n",
                   i, op->start_col, op->end_col, op_type, op->sequence_id);
        }
    }
    
    if (changes_found) {
        printf("Total local operations: %d/%d\n", local_op_count, MAX_OPERATIONS);
    }
}

void save_current_state() {
    for (int i = 0; i < MAX_LINES; i++) {
        if (prev_lines[i]) {
            free(prev_lines[i]);
            prev_lines[i] = NULL;
        }
        if (lines[i]) {
            prev_lines[i] = strdup(lines[i]);
        }
    }
}

void register_user(const char *user_id) {
    sem_wait(registry_sem);
    
    for (int i = 0; i < MAX_USERS; i++) {
        if (registry[i].active && strcmp(registry[i].user_id, user_id) == 0) {
            registry[i].last_heartbeat = time(NULL);
            sem_post(registry_sem);
            return;
        }
    }
    
    int registered = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!registry[i].active) {
            strncpy(registry[i].user_id, user_id, sizeof(registry[i].user_id) - 1);
            snprintf(registry[i].queue_name, sizeof(registry[i].queue_name), "/queue_%s", user_id);
            registry[i].active = 1;
            registry[i].last_heartbeat = time(NULL);
            registry[i].last_sequence = 0;
            registered = 1;
            printf("Registered %s\n", user_id);
            break;
        }
    }
    
    if (!registered) {
        printf("Registry full!\n");
        sem_post(registry_sem);
        exit(1);
    }
    
    sem_post(registry_sem);
}

void update_heartbeat() {
    sem_wait(registry_sem);
    for (int i = 0; i < MAX_USERS; i++) {
        if (registry[i].active && strcmp(registry[i].user_id, user_id) == 0) {
            registry[i].last_heartbeat = time(NULL);
            break;
        }
    }
    sem_post(registry_sem);
}

void unregister_user(const char *user_id) {
    sem_wait(registry_sem);
    for (int i = 0; i < MAX_USERS; i++) {
        if (registry[i].active && strcmp(registry[i].user_id, user_id) == 0) {
            registry[i].active = 0;
            printf("Unregistered %s\n", user_id);
            break;
        }
    }
    sem_post(registry_sem);
}

void get_active_users(char *active_users_str, size_t size) {
    sem_wait(registry_sem);
    
    active_users_str[0] = '\0';
    int first = 1;
    
    for (int i = 0; i < MAX_USERS; i++) {
        if (registry[i].active) {
            if (!first) {
                strncat(active_users_str, ", ", size - strlen(active_users_str) - 1);
            }
            strncat(active_users_str, registry[i].user_id, size - strlen(active_users_str) - 1);
            first = 0;
        }
    }
    
    if (first) {
        strncpy(active_users_str, "None", size);
    }
    
    sem_post(registry_sem);
}

void cleanup() {
    printf("\nCleaning up: %s\n", user_id);
    unregister_user(user_id);
    
    char queue_name[64];
    snprintf(queue_name, sizeof(queue_name), "/queue_%s", user_id);
    mq_close(my_queue);
    mq_unlink(queue_name);
    
    if (registry) {
        munmap(registry, sizeof(struct UserInfo) * MAX_USERS);
    }
    if (shm_fd != -1) {
        close(shm_fd);
    }
    if (registry_sem != SEM_FAILED) {
        sem_close(registry_sem);
    }
    
    for (int i = 0; i < MAX_LINES; i++) {
        if (lines[i]) free(lines[i]);
        if (prev_lines[i]) free(prev_lines[i]);
    }
    
    pthread_mutex_destroy(&merge_mutex);
}

void signal_handler(int sig) {
    running = 0;
}

void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    atexit(cleanup);
}

void clear_screen() {
    printf("\033[2J\033[1;1H");
}

void load_file() {
    for (int i = 0; i < MAX_LINES; i++) {
        if (lines[i]) {
            free(lines[i]);
            lines[i] = NULL;
        }
    }
    
    FILE *fp = fopen(filename, "r");
    if (!fp) return;
    
    char buffer[MAX_LEN];
    int i = 0;
    while (fgets(buffer, sizeof(buffer), fp) && i < MAX_LINES - 1) {
        buffer[strcspn(buffer, "\n")] = 0;
        lines[i] = strdup(buffer);
        i++;
    }
    fclose(fp);
}

void display_document() {
    clear_screen();
    time_t now = time(NULL);
    char active_users[256];
    
    get_active_users(active_users, sizeof(active_users));
    
    printf("=== SyncText CRDT Editor ===\n");
    printf("User: %s | Document: %s\n", user_id, filename);
    printf("Last updated: %s", ctime(&now));
    printf("Active users: %s\n", active_users);
    printf("Local ops: %d/%d | Received ops: %d\n", local_op_count, MAX_OPERATIONS, received_op_count);
    printf("----------------------------------------\n");
    
    int line_count = 0;
    for (int i = 0; i < MAX_LINES; i++) {
        if (lines[i] != NULL) {
            printf("Line %d: %s\n", i, lines[i]);
            line_count++;
        }
    }
    
    if (line_count == 0) {
        printf("(Document is empty)\n");
    }
    
    printf("----------------------------------------\n");
    printf("Monitoring for changes... (Ctrl+C to exit)\n");
    fflush(stdout);
}

void *monitor_file(void *arg) {
    struct stat attr;
    
    load_file();
    save_current_state();
    
    while (running) {
        if (stat(filename, &attr) == 0) {
            if (attr.st_mtime != last_mod_time) {
                printf("\nFile changed detected!\n");
                last_mod_time = attr.st_mtime;
                load_file();
                detect_changes();
                save_current_state();
                display_document();
            }
        }
        sleep(2);
    }
    return NULL;
}

void *heartbeat_thread(void *arg) {
    while (running) {
        update_heartbeat();
        sleep(3);
    }
    return NULL;
}

void *listener_thread(void *arg) {
    struct UpdateObject received_update;
    
    while (running) {
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 1;
        
        ssize_t bytes_read = mq_timedreceive(my_queue, (char*)&received_update, 
                                           sizeof(struct UpdateObject), NULL, &timeout);
        
        /*if (bytes_read == sizeof(struct UpdateObject)) {
            if (received_op_count < MAX_OPERATIONS * MAX_USERS) {
                received_operations[received_op_count] = received_update;
                received_op_count++;
                
                printf("Received: %s line %d type %d seq %d\n",
                       received_update.user_id, received_update.line_num,
                       received_update.type, received_update.sequence_id);
                
                printf("Received ops: %d\n", received_op_count);
            } else {
                printf("Receive buffer full!\n");
            }
        }*/

        if (bytes_read == sizeof(struct UpdateObject))
        {
            if (!already_applied(&received_update))
            { // <-- add this guard
                if (received_op_count < MAX_OPERATIONS * MAX_USERS)
                {
                    received_operations[received_op_count++] = received_update;
                    printf("Received: %s line %d type %d seq %d\n",
                           received_update.user_id, received_update.line_num,
                           received_update.type, received_update.sequence_id);
                }
                else
                {
                    printf("Receive buffer full!\n");
                }
            }
            else
            {
                // silently ignore duplicate
            }
        }
    }
    return NULL;
}

// CRDT: Check if we should merge based on CRDT conditions
int should_merge_crdt() {
    // Merge if we have enough operations or if we have pending received operations
    if (local_op_count >= MAX_OPERATIONS) return 1;
    if (received_op_count > 0) return 1;
    if (local_op_count + received_op_count >= MAX_OPERATIONS) return 1;
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: ./editor <user_id>\n");
        return 1;
    }

    strcpy(user_id, argv[1]);
    sprintf(filename, "%s_doc.txt", user_id);

    // Initialize arrays
    memset(lines, 0, sizeof(lines));
    memset(prev_lines, 0, sizeof(prev_lines));
    memset(local_operations, 0, sizeof(local_operations));
    memset(received_operations, 0, sizeof(received_operations));

    setup_signal_handlers();
    initialize_registry();
    create_message_queue();
    register_user(user_id);

    // Create initial document
    if (access(filename, F_OK) != 0) {
        FILE *fp = fopen(filename, "w");
        if (fp) {
            fprintf(fp, "Hello World\n");
            fprintf(fp, "This is a collaborative editor\n");
            fprintf(fp, "Welcome to SyncText\n");
            fprintf(fp, "Edit this document and see real-time updates\n");
            fclose(fp);
        }
    }

    load_file();
    save_current_state();
    display_document();

    pthread_t monitor_thread, heartbeat_thread_id, listener_thread_id;
    pthread_create(&monitor_thread, NULL, monitor_file, NULL);
    pthread_create(&heartbeat_thread_id, NULL, heartbeat_thread, NULL);
    pthread_create(&listener_thread_id, NULL, listener_thread, NULL);

    // Main CRDT loop
    time_t last_broadcast = time(NULL);
    time_t last_merge_check = time(NULL);
    
    while (running) {
        display_document();
        
        // CRDT: Check if we should merge (more frequent checking)
        time_t now = time(NULL);
        if (should_merge_crdt() && (now - last_merge_check >= 1)) {
            printf("\nCRDT: Triggering merge...\n");
            merge_operations_crdt();
            last_merge_check = now;
        }
        
        // Broadcast local operations periodically
        if (local_op_count > 0 && (now - last_broadcast >= 2)) {
            broadcast_accumulated_operations();
            last_broadcast = now;
        }
        
        sleep(1);
    }

    pthread_join(monitor_thread, NULL);
    pthread_join(heartbeat_thread_id, NULL);
    pthread_join(listener_thread_id, NULL);
    
    return 0;
}