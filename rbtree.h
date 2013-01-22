/* Copyright (c) 2011 the authors listed at the following URL, and/or
the authors of referenced articles or incorporated external code:
http://en.literateprograms.org/Red-black_tree_(C)?action=history&offset=20090121005050

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Retrieved from: http://en.literateprograms.org/Red-black_tree_(C)?oldid=16016
... and modified for even more speed and awesomeness...
*/

#ifndef _RBTREE_H_
#define _RBTREE_H_ 1
enum rbtree_node_color { RED, BLACK };

typedef struct rbtree_node_t {
    void* key;
    void* value;
    struct rbtree_node_t* left;
    struct rbtree_node_t* right;
    struct rbtree_node_t* parent;
    enum rbtree_node_color color;
} *rbtree_node;

typedef struct rbtree_t {
    rbtree_node root;
    rbtree_node last_visited_node;
    int nb_elements;
} *rbtree;

typedef struct key_val_arr {
   rbtree_node *vals;
   int nb_elements;
} rbtree_key_val_arr_t;

typedef int (*compare_func)(void* left, void* right);
typedef int (*sort_func)(const void* a, const void* b);
typedef int (*free_func)(rbtree_node n);

rbtree rbtree_create();
void* rbtree_lookup(rbtree t, void* key, compare_func compare);
void rbtree_insert(rbtree t, void* key, void* value, compare_func compare);
void rbtree_delete(rbtree t, void* key, compare_func compare);
void rbtree_print(rbtree t, compare_func show);
int pointer_cmp(void *left, void *right);
int pointer_cmp_reverse(void *left, void *right);
int int_cmp(void *left, void *right);
rbtree_key_val_arr_t *rbtree_sort(rbtree t, sort_func cmp);
void rbtree_free(rbtree t, free_func f);
void rbtree_arr_free(rbtree_key_val_arr_t *a);

#endif
