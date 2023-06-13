#include <stdlib.h>
#include "list.h"

struct list *cons(void *car, void *cdr) {
    struct list *l = malloc(sizeof(struct list));
    l->car = car;
    l->cdr = cdr;
    return l;
}
