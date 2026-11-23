#ifndef USER_USYNC_H
#define USER_USYNC_H

typedef struct {
  int locked;
} uspinlock_t;

void uspin_init(uspinlock_t *lk);
void uspin_lock(uspinlock_t *lk);
void uspin_unlock(uspinlock_t *lk);

typedef struct {
  int state;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/*
 * Counting semaphore based on futex.
 *
 * value > 0: available permits
 * value = 0: no permits available
 * value < 0: unsatisfied wait requests
 */
typedef struct {
  mutex_t lock;
  int value;
  int next_ticket;
  int served_ticket;
  int sequence;
} sem_t;

void sem_init(sem_t *s, int initial);
void sem_wait(sem_t *s);
void sem_post(sem_t *s);

#endif
