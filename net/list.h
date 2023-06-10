#pragma once

struct list {
    void *car;
    union {
        void *cdr;
        struct list *next;
    };
};

struct list *cons(void *car, void *cdr);
