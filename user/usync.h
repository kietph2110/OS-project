#ifndef USER_USYNC_H
#define USER_USYNC_H

typedef struct {
  int locked;
} uspinlock_t;

void uspin_init(uspinlock_t *lk);
void uspin_lock(uspinlock_t *lk);
void uspin_unlock(uspinlock_t *lk);

#endif
