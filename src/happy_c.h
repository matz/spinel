/* happy_c.h -- a little Ruby in the compiler's C.

   Loop forms named after the Ruby methods they read like. Each expands to a
   single plain `for`, so `break` and `continue` behave exactly as in the loop
   it replaces. Compiler-only: lib/ is compiled into user programs, where a
   lowercase `each` or `times` could collide with a name from Ruby source.

   A first cut, partially adopted: only codegen_stmt.c reads this way so far,
   and more Ruby-isms (unless, until, loop) can join as the code asks for them. */
#ifndef HAPPY_C_H
#define HAPPY_C_H

/* n.times { |i| ... }  --  n is re-read every turn, as the loops it replaces
   do: a walk over a table that grows under it must see the new entries. */
#define times(i, n) for (int i = 0; i < (n); i++)

/* lo.upto(hi) { |i| ... } */
#define upto(i, lo, hi) for (int i = (lo); i <= (hi); i++)

/* node.field.each { |v| ... }  --  v is each node id in the array field;
   v_i is its index and v_n the length (each_with_index). */
#define each(v, nt, id, field)                                                 \
  for (int v##_n = 0, *v##_a = (int *)nt_arr(nt, id, #field, &v##_n),       \
           v##_i = 0, v = 0;                                                  \
       v##_i < v##_n && ((v = v##_a[v##_i]), 1); v##_i++)

#endif
