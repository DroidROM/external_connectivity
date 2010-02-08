/*
** Copyright 2006, The Android Open Source Project
** Copyright (c) 2010, Code Aurora Forum. All rights reserved.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "CND_EVENT"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <utils/Log.h>
#include <cnd_event.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>

static pthread_mutex_t listMutex;
#define MUTEX_ACQUIRE() pthread_mutex_lock(&listMutex)
#define MUTEX_RELEASE() pthread_mutex_unlock(&listMutex)
#define MUTEX_INIT() pthread_mutex_init(&listMutex, NULL)
#define MUTEX_DESTROY() pthread_mutex_destroy(&listMutex)

extern "C" int cne_svc_init(void);

static fd_set readFds;
static int nfds = 0;

static struct cnd_event * watch_table[MAX_FD_EVENTS];
static struct cnd_event pending_list;

static void init_list(struct cnd_event * list)
{
    memset(list, 0, sizeof(struct cnd_event));
    list->next = list;
    list->prev = list;
    list->fd = -1;
}

static void addToList(struct cnd_event * ev, struct cnd_event * list)
{
    ev->next = list;
    ev->prev = list->prev;
    ev->prev->next = ev;
    list->prev = ev;

}

static void removeFromList(struct cnd_event * ev)
{

    ev->next->prev = ev->prev;
    ev->prev->next = ev->next;
    ev->next = NULL;
    ev->prev = NULL;
}


static void removeWatch(struct cnd_event * ev, int index)
{

    LOGD ("removeWatch: fd=%d, index=%d", ev->fd, index);

    watch_table[index] = NULL;
    ev->index = -1;

    FD_CLR(ev->fd, &readFds);

    if (ev->fd+1 == nfds) {
        int n = 0;

        for (int i = 0; i < MAX_FD_EVENTS; i++) {
            struct cnd_event * rev = watch_table[i];

            if ((rev != NULL) && (rev->fd > n)) {
                n = rev->fd;
            }
        }
        nfds = n + 1;

    }
}

static void processReadReadyEvent(fd_set * rfds, int n)
{

    LOGD ("processReadReadyEvent: n=%d, rfds0=%ld", n, rfds->fds_bits[0]);
    MUTEX_ACQUIRE();

    for (int i = 0; (i < MAX_FD_EVENTS) && (n > 0); i++) {

        struct cnd_event * rev = watch_table[i];

    if (rev != NULL)
           LOGD ("processReadReadyEvent: i=%d, fd=%d, rfds0=%ld", i, rev->fd, rfds->fds_bits[0]);
    else
       LOGD ("processReadReadyEvent: i=%d, rev is NULL", i);

        if (rev != NULL && FD_ISSET(rev->fd, rfds)) {
            addToList(rev, &pending_list);
            LOGD ("processReadReadyEvent: add to pendingList fd=%d", rev->fd);
            if (rev->persist == 0) {
                 removeWatch(rev, i);
            }
            n--;
        }
    }

    MUTEX_RELEASE();

}

static void firePendingEvent(void)
{

    struct cnd_event * ev = pending_list.next;
    while (ev != &pending_list) {
        struct cnd_event * next = ev->next;
        removeFromList(ev);
        ev->func(ev->fd, ev->param);
        ev = next;
    }

}

// Initialize internal data structs
int cnd_event_init()
{

    MUTEX_INIT();
    FD_ZERO(&readFds);
    init_list(&pending_list);
    memset(watch_table, 0, sizeof(watch_table));
    return(cne_svc_init());
}

// Initialize an event
void cnd_event_set(struct cnd_event * ev, int fd, int persist, cnd_event_cb func, void * param)
{
    memset(ev, 0, sizeof(struct cnd_event));

    ev->fd = fd;
    ev->index = -1;
    ev->persist = persist;
    ev->func = func;
    ev->param = param;

    fcntl(fd, F_SETFL, O_NONBLOCK);

}

// Add event to watch list
void cnd_event_add(struct cnd_event * ev)
{

    MUTEX_ACQUIRE();

    for (int i = 0; i < MAX_FD_EVENTS; i++) {
        if (watch_table[i] == NULL) {
            watch_table[i] = ev;
            ev->index = i;
               LOGD ("cnd_event_add-before: add at i=%d for fd=%d, readFds0=%ld", i, ev->fd, readFds.fds_bits[0]);

            FD_SET(ev->fd, &readFds);

            if (ev->fd >= nfds)
        nfds = ev->fd+1;
            LOGD ("cnd_event_add-after: add at i=%d for fd=%d, readFds0=%ld", i, ev->fd, readFds.fds_bits[0]);

            break;
        }
    }
    MUTEX_RELEASE();


}


// Remove event from watch or timer list
void cnd_event_del(struct cnd_event * ev)
{


    LOGD ("cnd_event_del: index=%d", ev->index);
    MUTEX_ACQUIRE();

    if (ev->index < 0 || ev->index >= MAX_FD_EVENTS) {
        return;
    }

    removeWatch(ev, ev->index);

    MUTEX_RELEASE();

}


void cnd_dump_watch_table(void)
{
   struct cnd_event * ev;
   for (int i = 0; i < MAX_FD_EVENTS; i++) {
        if (watch_table[i] != NULL) {
            ev = watch_table[i];
            LOGD ("cnd_dump_watch_table: at i=%d , fd=%d", i, ev->fd);
         }
   }

   return;
}

void cnd_event_loop(void)
{
    int n;
    fd_set rfds;
    int s_fdCommand;

    LOGD ("cnd_event_loop: started, nfds=%d",nfds);

    for (;;) {
      // make local copy of read fd_set
      memcpy(&rfds, &readFds, sizeof(fd_set));

      LOGD ("cnd_event_loop: waiting for select nfds=%d, rfds0=%ld", nfds, rfds.fds_bits[0]);

      n = select(nfds, &rfds, NULL, NULL, NULL);
      if (n < 0) {
        if (errno == EINTR)
          continue;
        LOGE("cnd_event_loop: select error (%d)", errno);
        return;
      }

      if (n == 0)
        LOGD ("cnd_event_loop: select timedout");
      else if (n > 0)
        LOGD ("cnd_event_loop: select ok,n=%d, rfds0=%ld",n, rfds.fds_bits[0]);

      // Check for read-ready events
      processReadReadyEvent(&rfds, n);
      // Fire pending event
      firePendingEvent();
    }
}